//===-- MachExceptionTest.cpp ---------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "DNBDefs.h"
#include "MachException.h"

#include <mach/mach_time.h>

#include <csignal>
#include <cstdint>
#include <ctime>
#include <deque>
#include <limits>
#include <string>
#include <thread>
#include <vector>

// The MIG client routine generated from mach_exc.defs, built into debugserver.
extern "C" kern_return_t
mach_exception_raise(mach_port_t exception_port, mach_port_t thread,
                     mach_port_t task, exception_type_t exception,
                     mach_exception_data_t code,
                     mach_msg_type_number_t code_count);

namespace {

DNBStopTimestamp MakeTimestamp(time_t sec, long nsec,
                               uint64_t continuous_time) {
  DNBStopTimestamp timestamp = {};
  timestamp.has_wall_time = true;
  timestamp.wall_time.tv_sec = sec;
  timestamp.wall_time.tv_nsec = nsec;
  timestamp.has_continuous_time = true;
  timestamp.continuous_time = continuous_time;
  return timestamp;
}

void ExpectSameTimestamp(const DNBStopTimestamp &actual,
                         const DNBStopTimestamp &expected) {
  EXPECT_EQ(actual.has_wall_time, expected.has_wall_time);
  EXPECT_EQ(actual.wall_time.tv_sec, expected.wall_time.tv_sec);
  EXPECT_EQ(actual.wall_time.tv_nsec, expected.wall_time.tv_nsec);
  EXPECT_EQ(actual.has_continuous_time, expected.has_continuous_time);
  EXPECT_EQ(actual.continuous_time, expected.continuous_time);
}

MachException::Data MakeException(exception_type_t type,
                                  std::vector<mach_exception_data_type_t> data,
                                  const DNBStopTimestamp &timestamp) {
  MachException::Data exc;
  exc.task_port = 1;
  exc.thread_port = 2;
  exc.exc_type = type;
  exc.exc_data = std::move(data);
  exc.timestamp = timestamp;
  return exc;
}

// Clock readings for Message::Receive to report, in the order received.
std::deque<DNBStopTimestamp> g_samples;

DNBStopTimestamp NextSample() {
  DNBStopTimestamp sample = g_samples.front();
  g_samples.pop_front();
  return sample;
}

// A port with a receive and a send right. Exception messages arrive on one;
// others stand in for the task and thread ports the kernel would send, which
// a process may not put in a message itself.
class Port {
public:
  Port() {
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &m_port);
    mach_port_insert_right(mach_task_self(), m_port, m_port,
                           MACH_MSG_TYPE_MAKE_SEND);
  }
  ~Port() {
    mach_port_deallocate(mach_task_self(), m_port);
    mach_port_mod_refs(mach_task_self(), m_port, MACH_PORT_RIGHT_RECEIVE, -1);
  }
  mach_port_t Get() const { return m_port; }

  // Receives the next message the way the exception thread does, and
  // decodes it for task.
  MachException::Message Receive(task_t task) {
    MachException::Message message;
    EXPECT_EQ(message.Receive(m_port, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 5000,
                              MACH_PORT_NULL, NextSample),
              KERN_SUCCESS);
    EXPECT_TRUE(message.CatchExceptionRaise(task));
    return message;
  }

private:
  mach_port_t m_port = MACH_PORT_NULL;
};

// Sends an exception message from another thread and waits for the reply,
// like a thread that raised an exception.
class RaisingThread {
public:
  RaisingThread(mach_port_t port, mach_port_t thread, mach_port_t task,
                exception_type_t type,
                std::vector<mach_exception_data_type_t> code)
      : m_thread([this, port, thread, task, type, code]() mutable {
          m_result = mach_exception_raise(port, thread, task, type, code.data(),
                                          code.size());
        }) {}
  ~RaisingThread() {
    if (m_thread.joinable())
      m_thread.join();
  }
  kern_return_t Join() {
    m_thread.join();
    return m_result;
  }

private:
  kern_return_t m_result = KERN_FAILURE;
  std::thread m_thread;
};

void Reply(MachException::Message &message) {
  mach_msg(&message.reply_msg.hdr, MACH_SEND_MSG,
           message.reply_msg.hdr.msgh_size, 0, MACH_PORT_NULL,
           MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

} // namespace

TEST(DNBStopTimestamp, WallTimeStringHasNineFractionDigits) {
  EXPECT_EQ(MakeTimestamp(1729500000, 0, 0).WallTimeString(),
            "1729500000.000000000");
  EXPECT_EQ(MakeTimestamp(1729500000, 1000, 0).WallTimeString(),
            "1729500000.000001000");
  EXPECT_EQ(MakeTimestamp(1, 5, 0).WallTimeString(), "1.000000005");
  EXPECT_EQ(MakeTimestamp(0, 999999999, 0).WallTimeString(), "0.999999999");
  const time_t max_seconds = std::numeric_limits<time_t>::max();
  EXPECT_EQ(MakeTimestamp(max_seconds, 999999999, 0).WallTimeString(),
            std::to_string(max_seconds) + ".999999999");
}

TEST(DNBStopTimestamp, StopReplyFieldsOmitUnavailableClocks) {
  DNBStopTimestamp timestamp = MakeTimestamp(1729500000, 1000, UINT64_MAX);
  EXPECT_EQ(timestamp.StopReplyFields(),
            "time:1729500000.000001000;conttime:18446744073709551615;");
  timestamp.has_continuous_time = false;
  EXPECT_EQ(timestamp.StopReplyFields(), "time:1729500000.000001000;");
  timestamp.has_continuous_time = true;
  timestamp.has_wall_time = false;
  EXPECT_EQ(timestamp.StopReplyFields(), "conttime:18446744073709551615;");
  timestamp.has_continuous_time = false;
  EXPECT_EQ(timestamp.StopReplyFields(), "");
}

TEST(MachExceptionClocks, KeepsBothReadings) {
  const struct timespec realtime = {1729500000, 123456000};
  ExpectSameTimestamp(MachException::MakeStopTimestamp(42, 0, realtime),
                      MakeTimestamp(1729500000, 123456000, 42));
}

TEST(MachExceptionClocks, FailedWallClockKeepsContinuousTime) {
  // Whatever clock_gettime() left in the output is not reported.
  const struct timespec garbage = {123, 456};
  DNBStopTimestamp timestamp =
      MachException::MakeStopTimestamp(42, -1, garbage);
  EXPECT_FALSE(timestamp.has_wall_time);
  EXPECT_EQ(timestamp.wall_time.tv_sec, 0);
  EXPECT_EQ(timestamp.wall_time.tv_nsec, 0);
  EXPECT_TRUE(timestamp.has_continuous_time);
  EXPECT_EQ(timestamp.continuous_time, 42u);
}

TEST(MachExceptionClocks, RejectsWallTimesItCannotPrint) {
  EXPECT_FALSE(MachException::MakeStopTimestamp(42, 0, {-1, 0}).has_wall_time);
  EXPECT_FALSE(
      MachException::MakeStopTimestamp(42, 0, {1, 1000000000}).has_wall_time);
  EXPECT_FALSE(MachException::MakeStopTimestamp(42, 0, {1, -1}).has_wall_time);
}

TEST(MachExceptionClocks, ZeroContinuousTimeIsAReading) {
  DNBStopTimestamp timestamp = MachException::MakeStopTimestamp(0, 0, {1, 0});
  EXPECT_TRUE(timestamp.has_continuous_time);
  EXPECT_EQ(timestamp.continuous_time, 0u);
}

TEST(MachExceptionClocks, WallTimeIsNotCorrected) {
  // A wall clock that was set back is reported as it reads.
  ExpectSameTimestamp(MachException::MakeStopTimestamp(10, 0, {2000, 0}),
                      MakeTimestamp(2000, 0, 10));
  ExpectSameTimestamp(MachException::MakeStopTimestamp(11, 0, {1000, 0}),
                      MakeTimestamp(1000, 0, 11));
}

TEST(MachExceptionClocks, SamplesThisHostsClocks) {
  struct timespec before_wall, after_wall;
  ASSERT_EQ(clock_gettime(CLOCK_REALTIME, &before_wall), 0);
  const uint64_t before_continuous = mach_continuous_time();
  DNBStopTimestamp timestamp = MachException::SampleStopTimestamp();
  const uint64_t after_continuous = mach_continuous_time();
  ASSERT_EQ(clock_gettime(CLOCK_REALTIME, &after_wall), 0);

  ASSERT_TRUE(timestamp.has_continuous_time);
  EXPECT_LE(before_continuous, timestamp.continuous_time);
  EXPECT_LE(timestamp.continuous_time, after_continuous);
  ASSERT_TRUE(timestamp.has_wall_time);
  auto ns = [](const struct timespec &ts) {
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
  };
  EXPECT_LE(ns(before_wall), ns(timestamp.wall_time));
  EXPECT_LE(ns(timestamp.wall_time), ns(after_wall));
}

TEST(MachExceptionData, StopInfoCarriesTheExceptionsTimestamp) {
  MachException::Data exc = MakeException(
      EXC_BREAKPOINT, {1, 0x1000}, MakeTimestamp(1729500000, 1000, UINT64_MAX));

  DNBThreadStopInfo stop_info;
  ASSERT_TRUE(exc.GetStopInfo(&stop_info));
  EXPECT_EQ(stop_info.reason, eStopTypeException);
  ExpectSameTimestamp(stop_info.timestamp,
                      MakeTimestamp(1729500000, 1000, UINT64_MAX));
}

TEST(MachExceptionData, NoExceptionNoTimestamp) {
  MachException::Data exc =
      MakeException(EXC_BREAKPOINT, {1, 0x1000}, MakeTimestamp(1, 0, 42));
  exc.exc_type = 0;

  DNBThreadStopInfo stop_info;
  ASSERT_TRUE(exc.GetStopInfo(&stop_info));
  EXPECT_EQ(stop_info.reason, eStopTypeInvalid);
  EXPECT_FALSE(stop_info.timestamp.has_wall_time);
  EXPECT_FALSE(stop_info.timestamp.has_continuous_time);
}

TEST(MachExceptionData, ClearDropsTheTimestamp) {
  MachException::Data exc =
      MakeException(EXC_BREAKPOINT, {1, 0x1000}, MakeTimestamp(1, 0, 42));
  exc.Clear();
  EXPECT_FALSE(exc.timestamp.has_wall_time);
  EXPECT_FALSE(exc.timestamp.has_continuous_time);
}

#if defined(__arm64__) || defined(__aarch64__)
// Watchpoint exceptions take an early return in GetStopInfo.
TEST(MachExceptionData, WatchpointStopInfoCarriesTheTimestamp) {
  MachException::Data exc = MakeException(
      EXC_BREAKPOINT, {EXC_ARM_DA_DEBUG, 0x2000}, MakeTimestamp(1, 0, 42));

  DNBThreadStopInfo stop_info;
  ASSERT_TRUE(exc.GetStopInfo(&stop_info));
  EXPECT_EQ(stop_info.reason, eStopTypeWatchpoint);
  ExpectSameTimestamp(stop_info.timestamp, MakeTimestamp(1, 0, 42));
}
#endif

TEST(MachExceptionData, StopReasonKeepsItsOwnTimestamp) {
  const DNBStopTimestamp first = MakeTimestamp(1, 1, 111);
  const DNBStopTimestamp second = MakeTimestamp(2, 2, 222);
  const DNBStopTimestamp third = MakeTimestamp(3, 3, 333);

  MachException::Data stop_exception;
  stop_exception.UpdateWithLaterException(
      MakeException(EXC_BREAKPOINT, {1, 0x1000}, first));
  ExpectSameTimestamp(stop_exception.timestamp, first);

  // A signal replaces a breakpoint, and brings its own time.
  stop_exception.UpdateWithLaterException(
      MakeException(EXC_SOFTWARE, {EXC_SOFT_SIGNAL, SIGSTOP}, second));
  EXPECT_EQ(stop_exception.exc_type, EXC_SOFTWARE);
  ExpectSameTimestamp(stop_exception.timestamp, second);

  // Nothing replaces the signal, or its time.
  stop_exception.UpdateWithLaterException(
      MakeException(EXC_BREAKPOINT, {1, 0x2000}, third));
  EXPECT_EQ(stop_exception.exc_type, EXC_SOFTWARE);
  ExpectSameTimestamp(stop_exception.timestamp, second);
}

TEST(MachExceptionMessage, EachMessageInABundleKeepsItsOwnSample) {
  // The wall and continuous readings differ between the samples, so a pair
  // made from two of them would show.
  const DNBStopTimestamp first = MakeTimestamp(1000, 1000, 2222);
  const DNBStopTimestamp second = MakeTimestamp(2000, 2000, 1111);
  g_samples = {first, second};

  Port port, task, thread_a_port, thread_b_port;
  RaisingThread thread_a(port.Get(), thread_a_port.Get(), task.Get(),
                         EXC_BREAKPOINT, {1, 0x1000});
  RaisingThread thread_b(port.Get(), thread_b_port.Get(), task.Get(),
                         EXC_BREAKPOINT, {1, 0x2000});

  // Both are received before either is answered, as one bundle.
  MachException::Message::collection bundle;
  bundle.push_back(port.Receive(task.Get()));
  bundle.push_back(port.Receive(task.Get()));

  ASSERT_TRUE(bundle[0].state.IsValid());
  ASSERT_TRUE(bundle[1].state.IsValid());
  EXPECT_NE(bundle[0].state.thread_port, bundle[1].state.thread_port);
  ExpectSameTimestamp(bundle[0].state.timestamp, first);
  ExpectSameTimestamp(bundle[1].state.timestamp, second);

  DNBThreadStopInfo stop_info;
  ASSERT_TRUE(bundle[0].state.GetStopInfo(&stop_info));
  ExpectSameTimestamp(stop_info.timestamp, first);
  ASSERT_TRUE(bundle[1].state.GetStopInfo(&stop_info));
  ExpectSameTimestamp(stop_info.timestamp, second);

  for (MachException::Message &message : bundle)
    Reply(message);
  EXPECT_EQ(thread_a.Join(), KERN_SUCCESS);
  EXPECT_EQ(thread_b.Join(), KERN_SUCCESS);
}

TEST(MachExceptionMessage, RejectedMessageChangesNothing) {
  const DNBStopTimestamp accepted_sample = MakeTimestamp(1000, 1000, 1111);
  const DNBStopTimestamp rejected_sample = MakeTimestamp(2000, 2000, 2222);
  g_samples = {accepted_sample, rejected_sample};

  Port port, task, thread_a_port, thread_b_port;
  RaisingThread ours(port.Get(), thread_a_port.Get(), task.Get(),
                     EXC_BREAKPOINT, {1, 0x1000});
  MachException::Message accepted = port.Receive(task.Get());

  // A message for a task other than the one being debugged is decoded but
  // not accepted. (That task must be real: the mismatch is checked with
  // task_info().)
  RaisingThread theirs(port.Get(), thread_b_port.Get(), task.Get(),
                       EXC_BREAKPOINT, {1, 0x2000});
  MachException::Message rejected = port.Receive(mach_task_self());
  EXPECT_FALSE(rejected.state.IsValid());

  ExpectSameTimestamp(accepted.state.timestamp, accepted_sample);
  DNBThreadStopInfo stop_info;
  ASSERT_TRUE(accepted.state.GetStopInfo(&stop_info));
  ExpectSameTimestamp(stop_info.timestamp, accepted_sample);
  ASSERT_TRUE(rejected.state.GetStopInfo(&stop_info));
  EXPECT_FALSE(stop_info.timestamp.has_wall_time);
  EXPECT_FALSE(stop_info.timestamp.has_continuous_time);

  Reply(accepted);
  Reply(rejected);
  EXPECT_EQ(ours.Join(), KERN_SUCCESS);
  EXPECT_NE(theirs.Join(), KERN_SUCCESS);
}
