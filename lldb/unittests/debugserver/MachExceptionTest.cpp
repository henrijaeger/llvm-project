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

#include <mach/mach_error.h>
#include <mach/mach_time.h>
#include <mach/mig_errors.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <future>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
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
  if (g_samples.empty()) {
    ADD_FAILURE() << "received more messages than the test has samples for";
    return {};
  }
  DNBStopTimestamp sample = g_samples.front();
  g_samples.pop_front();
  return sample;
}

std::string Describe(kern_return_t kr) {
  return std::string(mach_error_string(kr)) + " (" + std::to_string(kr) + ")";
}

std::string Hex(mach_port_t port) {
  std::ostringstream stream;
  stream << "0x" << std::hex << port;
  return stream.str();
}

// What shutting an ExceptionServer down did.
struct ShutdownReport {
  // What each sender's mach_exception_raise() returned, in the order they
  // were raised: KERN_SUCCESS if answered, MIG_SERVER_DIED if its request was
  // received or queued but not answered, MACH_SEND_INVALID_DEST if it had not
  // sent yet.
  std::vector<kern_return_t> results;
  // Every Mach call during shutdown that did not succeed.
  std::vector<std::string> errors;
};

// Receives exception messages that helper threads send, as debugserver
// receives the kernel's. The threads wait for replies, so whatever a test
// does, shutdown first releases every request that was not answered and only
// then waits for the threads.
class ExceptionServer {
public:
  // When a sender sends its request.
  enum class Start { Now, AtShutdown };

  // report, if given, receives what the destructor's shutdown did. Without
  // one, its errors are reported as test failures.
  explicit ExceptionServer(ShutdownReport *report = nullptr)
      : m_report(report) {
    m_port = NewPort();
  }

  ~ExceptionServer() {
    if (m_shut_down)
      return;
    ShutdownReport report = Shutdown();
    if (m_report) {
      *m_report = std::move(report);
      return;
    }
    for (const std::string &error : report.errors)
      ADD_FAILURE() << "shutdown: " << error;
  }

  // The port the server receives on.
  mach_port_t ReceivePort() const { return m_port; }

  // A port with a receive and a send right. Besides the one messages arrive
  // on, these stand in for the task and thread ports the kernel would send,
  // which a process may not put in a message itself.
  mach_port_t NewPort() {
    mach_port_t port = MACH_PORT_NULL;
    kern_return_t kr =
        mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port);
    EXPECT_EQ(kr, KERN_SUCCESS) << "mach_port_allocate: " << Describe(kr);
    if (kr != KERN_SUCCESS)
      return MACH_PORT_NULL;
    m_ports.push_back(port);
    kr = mach_port_insert_right(mach_task_self(), port, port,
                                MACH_MSG_TYPE_MAKE_SEND);
    EXPECT_EQ(kr, KERN_SUCCESS)
        << "mach_port_insert_right " << Hex(port) << ": " << Describe(kr);
    return port;
  }

  // Sends an exception message from a new thread, which waits for a reply
  // the way a thread that raised an exception does. With Start::AtShutdown,
  // the thread sends only after shutdown has destroyed the server's ports.
  void Raise(mach_port_t thread, mach_port_t task, exception_type_t type,
             std::vector<mach_exception_data_type_t> code,
             Start start = Start::Now) {
    auto sender = std::make_unique<Sender>();
    Sender *s = sender.get();
    std::shared_future<void> go;
    if (start == Start::AtShutdown)
      go = s->go.get_future().share();
    const mach_port_t port = m_port;
    s->thread = std::thread([s, go, port, thread, task, type, code]() mutable {
      if (go.valid())
        go.wait();
      s->result = mach_exception_raise(port, thread, task, type, code.data(),
                                       code.size());
    });
    s->held = start == Start::AtShutdown;
    m_senders.push_back(std::move(sender));
  }

  // Waits until exactly count messages are queued on the receive port, so a
  // test knows a request was sent but not received.
  testing::AssertionResult WaitUntilQueued(natural_t count) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    mach_port_status_t status = {};
    do {
      mach_msg_type_number_t size = MACH_PORT_RECEIVE_STATUS_COUNT;
      kern_return_t kr = mach_port_get_attributes(
          mach_task_self(), m_port, MACH_PORT_RECEIVE_STATUS,
          reinterpret_cast<mach_port_info_t>(&status), &size);
      if (kr != KERN_SUCCESS)
        return testing::AssertionFailure()
               << "mach_port_get_attributes " << Hex(m_port) << ": "
               << Describe(kr);
      if (status.mps_msgcount == count)
        return testing::AssertionSuccess();
      std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return testing::AssertionFailure()
           << "expected " << count << " queued messages on " << Hex(m_port)
           << " within 5 s, found " << status.mps_msgcount;
  }

  // Receives the next message within timeout_ms and decodes it for task.
  // Nothing is decoded if nothing was received, and a message that is not an
  // exception request is destroyed instead.
  testing::AssertionResult Receive(task_t task, MachException::Message &message,
                                   mach_msg_timeout_t timeout_ms = 5000) {
    kern_return_t kr = message.Receive(m_port, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                                       timeout_ms, MACH_PORT_NULL, NextSample);
    if (kr != KERN_SUCCESS)
      return testing::AssertionFailure() << "receive: " << Describe(kr);
    if (!message.CatchExceptionRaise(task)) {
      const mach_msg_id_t id = message.exc_msg.hdr.msgh_id;
      mach_msg_destroy(&message.exc_msg.hdr);
      return testing::AssertionFailure()
             << "message " << id << " is not an exception request";
    }
    m_unanswered.insert(message.reply_msg.hdr.msgh_remote_port);
    return testing::AssertionSuccess();
  }

  // Sends the reply that the decoded message's sender is waiting for.
  testing::AssertionResult Reply(MachException::Message &message) {
    const mach_port_t reply_port = message.reply_msg.hdr.msgh_remote_port;
    if (!m_unanswered.count(reply_port))
      return testing::AssertionFailure() << "no request waits for this reply";
    kern_return_t kr =
        mach_msg(&message.reply_msg.hdr, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
                 message.reply_msg.hdr.msgh_size, 0, MACH_PORT_NULL, 5000,
                 MACH_PORT_NULL);
    if (kr != KERN_SUCCESS)
      return testing::AssertionFailure() << "reply: " << Describe(kr);
    m_unanswered.erase(reply_port);
    return testing::AssertionSuccess();
  }

  // Releases every request that was not answered, waits for all senders,
  // and releases the server's ports. Every step is attempted even if an
  // earlier one fails. A later call does nothing.
  ShutdownReport Shutdown() {
    ShutdownReport report;
    if (m_shut_down)
      return report;
    m_shut_down = true;
    auto check = [&report](kern_return_t kr, const std::string &what) {
      if (kr != KERN_SUCCESS)
        report.errors.push_back(what + ": " + Describe(kr));
    };
    // Dropping the reply right of a received request wakes its sender.
    for (mach_port_t reply_port : m_unanswered)
      check(mach_port_deallocate(mach_task_self(), reply_port),
            "deallocate reply right " + Hex(reply_port));
    m_unanswered.clear();
    // Dropping the receive rights discards the requests still queued, which
    // wakes their senders too. The send rights become dead names, so the
    // names cannot be reused while senders that have not sent yet use them.
    for (mach_port_t port : m_ports)
      check(mach_port_mod_refs(mach_task_self(), port, MACH_PORT_RIGHT_RECEIVE,
                               -1),
            "release receive right " + Hex(port));
    for (std::unique_ptr<Sender> &sender : m_senders)
      if (sender->held)
        sender->go.set_value();
    for (std::unique_ptr<Sender> &sender : m_senders) {
      sender->thread.join();
      report.results.push_back(sender->result);
    }
    m_senders.clear();
    for (mach_port_t port : m_ports) {
      mach_port_urefs_t refs = 0;
      kern_return_t kr = mach_port_get_refs(mach_task_self(), port,
                                            MACH_PORT_RIGHT_DEAD_NAME, &refs);
      check(kr, "count dead-name references of " + Hex(port));
      if (kr == KERN_SUCCESS && refs > 0)
        check(mach_port_mod_refs(mach_task_self(), port,
                                 MACH_PORT_RIGHT_DEAD_NAME,
                                 -static_cast<mach_port_delta_t>(refs)),
              "release dead name " + Hex(port));
    }
    m_ports.clear();
    return report;
  }

private:
  struct Sender {
    std::thread thread;
    bool held = false;
    std::promise<void> go;
    kern_return_t result = KERN_FAILURE;
  };

  ShutdownReport *m_report;
  bool m_shut_down = false;
  mach_port_t m_port = MACH_PORT_NULL;
  std::vector<mach_port_t> m_ports;
  std::set<mach_port_t> m_unanswered;
  std::vector<std::unique_ptr<Sender>> m_senders;
};

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
  // The same availability check as SampleStopTimestamp(). Without the
  // clocks, it reports neither.
  if (__builtin_available(macOS 10.12, iOS 10.0, tvOS 10.0, watchOS 3.0, *)) {
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
  } else {
    DNBStopTimestamp timestamp = MachException::SampleStopTimestamp();
    EXPECT_FALSE(timestamp.has_wall_time);
    EXPECT_FALSE(timestamp.has_continuous_time);
  }
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

  ExceptionServer server;
  const mach_port_t task = server.NewPort();
  server.Raise(server.NewPort(), task, EXC_BREAKPOINT, {1, 0x1000});
  server.Raise(server.NewPort(), task, EXC_BREAKPOINT, {1, 0x2000});

  // Both are received before either is answered, as one bundle.
  MachException::Message::collection bundle(2);
  ASSERT_TRUE(server.Receive(task, bundle[0]));
  ASSERT_TRUE(server.Receive(task, bundle[1]));

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
    EXPECT_TRUE(server.Reply(message));
  ShutdownReport report = server.Shutdown();
  EXPECT_EQ(report.results,
            std::vector<kern_return_t>({KERN_SUCCESS, KERN_SUCCESS}));
  EXPECT_EQ(report.errors, std::vector<std::string>());
}

TEST(MachExceptionMessage, RejectedMessageChangesNothing) {
  const DNBStopTimestamp accepted_sample = MakeTimestamp(1000, 1000, 1111);
  const DNBStopTimestamp rejected_sample = MakeTimestamp(2000, 2000, 2222);
  g_samples = {accepted_sample, rejected_sample};

  ExceptionServer server;
  const mach_port_t task = server.NewPort();
  server.Raise(server.NewPort(), task, EXC_BREAKPOINT, {1, 0x1000});
  MachException::Message accepted;
  ASSERT_TRUE(server.Receive(task, accepted));

  // A message for a task other than the one being debugged is decoded but
  // not accepted. (That task must be real: the mismatch is checked with
  // task_info().)
  server.Raise(server.NewPort(), task, EXC_BREAKPOINT, {1, 0x2000});
  MachException::Message rejected;
  ASSERT_TRUE(server.Receive(mach_task_self(), rejected));
  EXPECT_FALSE(rejected.state.IsValid());

  ExpectSameTimestamp(accepted.state.timestamp, accepted_sample);
  DNBThreadStopInfo stop_info;
  ASSERT_TRUE(accepted.state.GetStopInfo(&stop_info));
  ExpectSameTimestamp(stop_info.timestamp, accepted_sample);
  ASSERT_TRUE(rejected.state.GetStopInfo(&stop_info));
  EXPECT_FALSE(stop_info.timestamp.has_wall_time);
  EXPECT_FALSE(stop_info.timestamp.has_continuous_time);

  EXPECT_TRUE(server.Reply(accepted));
  EXPECT_TRUE(server.Reply(rejected));
  ShutdownReport report = server.Shutdown();
  ASSERT_EQ(report.results.size(), 2u);
  EXPECT_EQ(report.results[0], KERN_SUCCESS);
  EXPECT_EQ(report.results[1], KERN_FAILURE);
  EXPECT_EQ(report.errors, std::vector<std::string>());
}

TEST(ExceptionServer, NothingReceivedNothingDecoded) {
  g_samples = {MakeTimestamp(1, 0, 1)};
  ExceptionServer server;
  MachException::Message message;
  testing::AssertionResult received =
      server.Receive(server.NewPort(), message, /*timeout_ms=*/10);
  EXPECT_FALSE(received);
  EXPECT_NE(std::string(received.message()).find("receive:"), std::string::npos)
      << received.message();
  // The clocks were not read and nothing was decoded.
  EXPECT_EQ(g_samples.size(), 1u);
  EXPECT_FALSE(message.state.IsValid());
  EXPECT_FALSE(message.state.timestamp.has_continuous_time);
}

TEST(ExceptionServer, OtherMessagesAreNotDecoded) {
  g_samples = {MakeTimestamp(1, 0, 1)};
  ExceptionServer server;
  mach_msg_header_t header = {};
  header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
  header.msgh_size = sizeof(header);
  header.msgh_remote_port = server.ReceivePort();
  header.msgh_id = 1234;
  ASSERT_EQ(mach_msg(&header, MACH_SEND_MSG, sizeof(header), 0, MACH_PORT_NULL,
                     MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL),
            KERN_SUCCESS);

  MachException::Message message;
  testing::AssertionResult received = server.Receive(server.NewPort(), message);
  EXPECT_FALSE(received);
  EXPECT_NE(std::string(received.message()).find("not an exception request"),
            std::string::npos)
      << received.message();
  EXPECT_FALSE(message.state.IsValid());
}

namespace {
// Leaves one request in each state a sender can be in at shutdown: answered,
// received but not answered, queued but not received, and not sent yet.
// Returns whether every step of the setup worked.
testing::AssertionResult LeaveRequestsInEveryState(ExceptionServer &server) {
  g_samples = {MakeTimestamp(1, 0, 1), MakeTimestamp(2, 0, 2)};
  const mach_port_t task = server.NewPort();

  server.Raise(server.NewPort(), task, EXC_BREAKPOINT, {1, 0x1000});
  MachException::Message answered;
  testing::AssertionResult ok = server.Receive(task, answered);
  if (!ok)
    return ok;
  if (!(ok = server.Reply(answered)))
    return ok;

  server.Raise(server.NewPort(), task, EXC_BREAKPOINT, {1, 0x2000});
  MachException::Message unanswered;
  if (!(ok = server.Receive(task, unanswered)))
    return ok;

  server.Raise(server.NewPort(), task, EXC_BREAKPOINT, {1, 0x3000});
  if (!(ok = server.WaitUntilQueued(1)))
    return ok;

  server.Raise(server.NewPort(), task, EXC_BREAKPOINT, {1, 0x4000},
               ExceptionServer::Start::AtShutdown);
  return testing::AssertionSuccess();
}

const std::vector<kern_return_t> kEveryStateResults = {
    KERN_SUCCESS, MIG_SERVER_DIED, MIG_SERVER_DIED, MACH_SEND_INVALID_DEST};

// Writes the outcome to stderr, where the death test's parent reads it, and
// exits successfully only if the setup worked and the shutdown released every
// sender as expected without errors.
[[noreturn]] void ExitWithReport(const std::string &setup,
                                 const ShutdownReport &report) {
  fprintf(stderr, "setup: %s\nresults:", setup.c_str());
  for (kern_return_t result : report.results)
    fprintf(stderr, " %d", result);
  fprintf(stderr, "\nerrors: %zu\n", report.errors.size());
  for (const std::string &error : report.errors)
    fprintf(stderr, "error: %s\n", error.c_str());
  const bool ok = setup == "ok" && report.results == kEveryStateResults &&
                  report.errors.empty();
  std::_Exit(ok ? 0 : 1);
}

std::string ExpectedReport() {
  std::string expected = "setup: ok\nresults:";
  for (kern_return_t result : kEveryStateResults)
    expected += " " + std::to_string(result);
  return expected + "\nerrors: 0\n";
}

std::string Describe(const testing::AssertionResult &result) {
  return result ? "ok" : std::string("failed: ") + result.message();
}

// A test that fails with requests in every state.
void FailWithRequestsInEveryState(ShutdownReport &report, std::string &setup) {
  ExceptionServer server(&report);
  testing::AssertionResult ok = LeaveRequestsInEveryState(server);
  setup = Describe(ok);
  ASSERT_TRUE(ok);
  ASSERT_TRUE(false) << "injected early return";
}
} // namespace

// Shutdown runs in a child process that must finish within the bound; a hang
// ends it with SIGALRM, which fails these tests.
TEST(ExceptionServer, ShutdownReleasesSendersInEveryState) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(
      {
        alarm(10);
        std::string setup;
        ShutdownReport report;
        {
          ExceptionServer server;
          setup = Describe(LeaveRequestsInEveryState(server));
          report = server.Shutdown();
          // Destroying the server after shutting it down is safe.
        }
        ExitWithReport(setup, report);
      },
      testing::ExitedWithCode(0), ExpectedReport());
}

TEST(ExceptionServer, EarlyReturnReleasesSendersInEveryState) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_EXIT(
      {
        alarm(10);
        ShutdownReport report;
        std::string setup = "not run";
        FailWithRequestsInEveryState(report, setup);
        ExitWithReport(setup, report);
      },
      testing::ExitedWithCode(0), ExpectedReport());
}
