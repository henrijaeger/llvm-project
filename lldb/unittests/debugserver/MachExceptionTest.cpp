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

#include <climits>
#include <cstdint>

static DNBStopTimestamp MakeTimestamp(time_t sec, long nsec,
                                      uint64_t continuous_time) {
  DNBStopTimestamp timestamp = {};
  timestamp.has_wall_time = true;
  timestamp.wall_time.tv_sec = sec;
  timestamp.wall_time.tv_nsec = nsec;
  timestamp.has_continuous_time = true;
  timestamp.continuous_time = continuous_time;
  return timestamp;
}

static MachException::Data MakeBreakpointException() {
  MachException::Data data;
  data.task_port = 1;
  data.thread_port = 2;
  data.exc_type = EXC_BREAKPOINT;
  data.exc_data = {1, 0x1000};
  return data;
}

TEST(DNBStopTimestamp, WallTimeStringHasNineFractionDigits) {
  EXPECT_EQ(MakeTimestamp(1729500000, 0, 0).WallTimeString(),
            "1729500000.000000000");
  EXPECT_EQ(MakeTimestamp(1729500000, 1000, 0).WallTimeString(),
            "1729500000.000001000");
  EXPECT_EQ(MakeTimestamp(1, 5, 0).WallTimeString(), "1.000000005");
  EXPECT_EQ(MakeTimestamp(0, 999999999, 0).WallTimeString(), "0.999999999");
  EXPECT_EQ(MakeTimestamp(LONG_MAX, 999999999, 0).WallTimeString(),
            "9223372036854775807.999999999");
}

TEST(MachExceptionData, StopInfoCarriesTheExceptionsTimestamp) {
  MachException::Data data = MakeBreakpointException();
  data.timestamp = MakeTimestamp(1729500000, 123456000, UINT64_MAX);

  DNBThreadStopInfo stop_info;
  ASSERT_TRUE(data.GetStopInfo(&stop_info));
  EXPECT_EQ(stop_info.reason, eStopTypeException);
  EXPECT_TRUE(stop_info.timestamp.has_wall_time);
  EXPECT_EQ(stop_info.timestamp.wall_time.tv_sec, 1729500000);
  EXPECT_EQ(stop_info.timestamp.wall_time.tv_nsec, 123456000);
  EXPECT_TRUE(stop_info.timestamp.has_continuous_time);
  EXPECT_EQ(stop_info.timestamp.continuous_time, UINT64_MAX);
}

TEST(MachExceptionData, UnavailableClockStaysUnavailable) {
  MachException::Data data = MakeBreakpointException();
  data.timestamp = MakeTimestamp(0, 0, 42);
  data.timestamp.has_wall_time = false;

  DNBThreadStopInfo stop_info;
  ASSERT_TRUE(data.GetStopInfo(&stop_info));
  EXPECT_FALSE(stop_info.timestamp.has_wall_time);
  EXPECT_TRUE(stop_info.timestamp.has_continuous_time);
  EXPECT_EQ(stop_info.timestamp.continuous_time, 42u);
}

TEST(MachExceptionData, ClearDropsTheTimestamp) {
  MachException::Data data = MakeBreakpointException();
  data.timestamp = MakeTimestamp(1729500000, 0, 42);
  data.Clear();

  DNBThreadStopInfo stop_info;
  ASSERT_TRUE(data.GetStopInfo(&stop_info));
  EXPECT_EQ(stop_info.reason, eStopTypeInvalid);
  EXPECT_FALSE(stop_info.timestamp.has_wall_time);
  EXPECT_FALSE(stop_info.timestamp.has_continuous_time);
}

#if defined(__arm64__) || defined(__aarch64__)
// Watchpoint exceptions take an early return in GetStopInfo.
TEST(MachExceptionData, WatchpointStopInfoCarriesTheTimestamp) {
  MachException::Data data = MakeBreakpointException();
  data.exc_data = {EXC_ARM_DA_DEBUG, 0x2000};
  data.timestamp = MakeTimestamp(1729500000, 0, 42);

  DNBThreadStopInfo stop_info;
  ASSERT_TRUE(data.GetStopInfo(&stop_info));
  EXPECT_EQ(stop_info.reason, eStopTypeWatchpoint);
  EXPECT_TRUE(stop_info.timestamp.has_continuous_time);
  EXPECT_EQ(stop_info.timestamp.continuous_time, 42u);
}
#endif
