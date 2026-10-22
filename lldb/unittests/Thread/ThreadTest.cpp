//===-- ThreadTest.cpp ------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "lldb/Target/Thread.h"
#include "Plugins/Platform/Linux/PlatformLinux.h"
#include <thread>
#ifdef _WIN32
#include "lldb/Host/windows/HostThreadWindows.h"
#include "lldb/Host/windows/windows.h"

#include "Plugins/Platform/Windows/PlatformWindows.h"
#include "Plugins/Process/Windows/Common/LocalDebugDelegate.h"
#include "Plugins/Process/Windows/Common/ProcessWindows.h"
#include "Plugins/Process/Windows/Common/TargetThreadWindows.h"
#endif
#include "lldb/Core/Debugger.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Host/HostThread.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/StopInfo.h"
#include "lldb/Target/ThreadPlanBase.h"
#include "lldb/Utility/ArchSpec.h"
#include "lldb/Utility/StreamString.h"
#include "lldb/Utility/StructuredData.h"
#include "gtest/gtest.h"

using namespace lldb_private;
using namespace lldb;

namespace {

#ifdef _WIN32
using SetThreadDescriptionFunctionPtr =
    HRESULT(WINAPI *)(HANDLE hThread, PCWSTR lpThreadDescription);

static SetThreadDescriptionFunctionPtr SetThreadName;
#endif

class ThreadTest : public ::testing::Test {
public:
  void SetUp() override {
    FileSystem::Initialize();
    HostInfo::Initialize();
#ifdef _WIN32
    HMODULE hModule = ::LoadLibraryW(L"Kernel32.dll");
    if (hModule) {
      SetThreadName = reinterpret_cast<SetThreadDescriptionFunctionPtr>(
          (void *)::GetProcAddress(hModule, "SetThreadDescription"));
    }
    PlatformWindows::Initialize();
#endif
    platform_linux::PlatformLinux::Initialize();
  }
  void TearDown() override {
#ifdef _WIN32
    PlatformWindows::Terminate();
#endif
    platform_linux::PlatformLinux::Terminate();
    HostInfo::Terminate();
    FileSystem::Terminate();
  }
};

class DummyProcess : public Process {
public:
  DummyProcess(lldb::TargetSP target_sp, lldb::ListenerSP listener_sp)
      : Process(target_sp, listener_sp) {}

  bool CanDebug(lldb::TargetSP target, bool plugin_specified_by_name) override {
    return true;
  }
  Status DoDestroy() override { return {}; }
  void RefreshStateAfterStop() override {}
  size_t DoReadMemory(const ProcessAddress &process_addr, void *buf,
                      size_t size, Status &error) override {
    return 0;
  }
  bool DoUpdateThreadList(ThreadList &old_thread_list,
                          ThreadList &new_thread_list) override {
    return false;
  }
  llvm::StringRef GetPluginName() override { return "Dummy"; }

  ProcessModID &GetModIDNonConstRef() { return m_mod_id; }
};

class DummyThread : public Thread {
public:
  using Thread::Thread;

  ~DummyThread() override { DestroyThread(); }

  void RefreshStateAfterStop() override {}

  lldb::RegisterContextSP GetRegisterContext() override { return nullptr; }

  lldb::RegisterContextSP
  CreateRegisterContextForFrame(StackFrame *frame) override {
    return nullptr;
  }

  bool CalculateStopInfo() override { return false; }

  bool IsStillAtLastBreakpointHit() override { return true; }

  using Thread::PopPlan;
  using Thread::PushPlan;

  lldb::addr_t GetStoppedAtUnexpectedBP() const {
    return m_stopped_at_unexecuted_bp;
  }
};

class DummyThreadPlan : public ThreadPlanBase {
public:
  DummyThreadPlan(Thread &thread) : ThreadPlanBase(thread) {}
  ~DummyThreadPlan() override = default;
};
} // namespace

TargetSP CreateTarget(DebuggerSP &debugger_sp, ArchSpec &arch) {
  PlatformSP platform_sp;
  TargetSP target_sp;
  debugger_sp->GetTargetList().CreateTarget(
      *debugger_sp, "", arch, eLoadDependentsNo, platform_sp, target_sp);

  return target_sp;
}

#ifdef _WIN32
std::shared_ptr<TargetThreadWindows>
CreateWindowsThread(const ProcessWindowsSP &process_sp, std::thread &t) {
  HostThread host_thread((lldb::thread_t)t.native_handle());
  ThreadSP thread_sp =
      std::make_shared<TargetThreadWindows>(*process_sp.get(), host_thread);
  return std::static_pointer_cast<TargetThreadWindows>(thread_sp);
}

TEST_F(ThreadTest, GetThreadDescription) {
  if (!SetThreadName)
    return;

  ArchSpec arch(HostInfo::GetArchitecture());
  Platform::SetHostPlatform(PlatformWindows::CreateInstance(true, &arch));

  DebuggerSP debugger_sp = Debugger::CreateInstance();
  ASSERT_TRUE(debugger_sp);

  TargetSP target_sp = CreateTarget(debugger_sp, arch);
  ASSERT_TRUE(target_sp);

  ListenerSP listener_sp(Listener::MakeListener("dummy"));
  auto process_sp = std::static_pointer_cast<ProcessWindows>(
      ProcessWindows::CreateInstance(target_sp, listener_sp, nullptr, false));
  ASSERT_TRUE(process_sp);

  std::thread t([]() {});
  auto thread_sp = CreateWindowsThread(process_sp, t);
  DWORD tid = thread_sp->GetHostThread().GetNativeThread().GetThreadId();
  HANDLE hThread = ::OpenThread(THREAD_SET_LIMITED_INFORMATION, FALSE, tid);
  ASSERT_TRUE(hThread);

  SetThreadName(hThread, L"thread name");
  ::CloseHandle(hThread);
  ASSERT_STREQ(thread_sp->GetName(), "thread name");

  t.join();
}
#endif

TEST_F(ThreadTest, SetStopInfo) {
  ArchSpec arch("powerpc64-pc-linux");

  Platform::SetHostPlatform(
      platform_linux::PlatformLinux::CreateInstance(true, &arch));

  DebuggerSP debugger_sp = Debugger::CreateInstance();
  ASSERT_TRUE(debugger_sp);

  TargetSP target_sp = CreateTarget(debugger_sp, arch);
  ASSERT_TRUE(target_sp);

  ListenerSP listener_sp(Listener::MakeListener("dummy"));
  ProcessSP process_sp = std::make_shared<DummyProcess>(target_sp, listener_sp);
  ASSERT_TRUE(process_sp);

  DummyProcess *process = static_cast<DummyProcess *>(process_sp.get());

  ThreadSP thread_sp = std::make_shared<DummyThread>(*process_sp.get(), 0);
  ASSERT_TRUE(thread_sp);

  StopInfoSP stopinfo_sp =
      StopInfo::CreateStopReasonWithBreakpointSiteID(*thread_sp.get(), 0);
  ASSERT_TRUE(stopinfo_sp->IsValid() == true);

  /*
   Should make stopinfo valid.
   */
  process->GetModIDNonConstRef().BumpStopID();
  ASSERT_TRUE(stopinfo_sp->IsValid() == false);

  thread_sp->SetStopInfo(stopinfo_sp);
  ASSERT_TRUE(stopinfo_sp->IsValid() == true);
}

TEST_F(ThreadTest, GetPrivateStopInfo) {
  ArchSpec arch("powerpc64-pc-linux");

  Platform::SetHostPlatform(
      platform_linux::PlatformLinux::CreateInstance(true, &arch));

  DebuggerSP debugger_sp = Debugger::CreateInstance();
  ASSERT_TRUE(debugger_sp);

  TargetSP target_sp = CreateTarget(debugger_sp, arch);
  ASSERT_TRUE(target_sp);

  ListenerSP listener_sp(Listener::MakeListener("dummy"));
  ProcessSP process_sp = std::make_shared<DummyProcess>(target_sp, listener_sp);
  ASSERT_TRUE(process_sp);

  DummyProcess *process = static_cast<DummyProcess *>(process_sp.get());

  ThreadSP thread_sp = std::make_shared<DummyThread>(*process_sp.get(), 0);
  ASSERT_TRUE(thread_sp);

  StopInfoSP stopinfo_sp =
      StopInfo::CreateStopReasonWithBreakpointSiteID(*thread_sp.get(), 0);
  ASSERT_TRUE(stopinfo_sp);

  thread_sp->SetStopInfo(stopinfo_sp);

  /*
   Should make stopinfo valid if thread is at last breakpoint hit.
   */
  process->GetModIDNonConstRef().BumpStopID();
  ASSERT_TRUE(stopinfo_sp->IsValid() == false);
  StopInfoSP new_stopinfo_sp = thread_sp->GetPrivateStopInfo();
  ASSERT_TRUE(new_stopinfo_sp && stopinfo_sp->IsValid() == true);
}

TEST_F(ThreadTest, ShouldResume) {
  ArchSpec arch("x86_64-pc-linux");

  Platform::SetHostPlatform(
      platform_linux::PlatformLinux::CreateInstance(true, &arch));

  DebuggerSP debugger_sp = Debugger::CreateInstance();
  ASSERT_TRUE(debugger_sp);

  TargetSP target_sp = CreateTarget(debugger_sp, arch);
  ASSERT_TRUE(target_sp);

  ListenerSP listener_sp(Listener::MakeListener("dummy"));
  ProcessSP process_sp = std::make_shared<DummyProcess>(target_sp, listener_sp);
  ASSERT_TRUE(process_sp);

  auto thread_sp = std::make_shared<DummyThread>(*process_sp.get(), 0);
  ASSERT_TRUE(thread_sp);

  thread_sp->PushPlan(std::make_shared<DummyThreadPlan>(*thread_sp));
  ASSERT_TRUE(thread_sp->GetCurrentPlan());

  const lldb::addr_t unexpected_bp_addr = 0x1234;

  // If a thread is resumed with 'eStateRunning' or 'eStateStepping', it should
  // clear the address of an unexpected breakpoint.
  thread_sp->SetThreadStoppedAtUnexecutedBP(unexpected_bp_addr);
  EXPECT_TRUE(thread_sp->ShouldResume(eStateRunning));
  EXPECT_EQ(LLDB_INVALID_ADDRESS, thread_sp->GetStoppedAtUnexpectedBP());

  thread_sp->SetThreadStoppedAtUnexecutedBP(unexpected_bp_addr);
  EXPECT_TRUE(thread_sp->ShouldResume(eStateStepping));
  EXPECT_EQ(LLDB_INVALID_ADDRESS, thread_sp->GetStoppedAtUnexpectedBP());

  // If a thread is resumed with 'eStateSuspended', the address of an unexpected
  // breakpoint should be preserved.
  thread_sp->SetThreadStoppedAtUnexecutedBP(unexpected_bp_addr);
  EXPECT_TRUE(thread_sp->ShouldResume(eStateSuspended));
  EXPECT_EQ(unexpected_bp_addr, thread_sp->GetStoppedAtUnexpectedBP());
}

namespace {
// A stop info that carries extended info from its plugin.
class ExtendedInfoStopInfo : public StopInfo {
public:
  ExtendedInfoStopInfo(Thread &thread, StructuredData::ObjectSP info)
      : StopInfo(thread, 0) {
    m_extended_info = std::move(info);
  }
  StopReason GetStopReason() const override { return eStopReasonException; }
};

StopTimestamps MakeStopTimestamps(std::optional<std::string> time,
                                  std::optional<std::string> conttime) {
  StopTimestamps timestamps;
  timestamps.time = std::move(time);
  timestamps.conttime = std::move(conttime);
  return timestamps;
}

std::string ToJSON(const StructuredData::ObjectSP &object) {
  if (!object)
    return "<null>";
  StreamString stream;
  object->Dump(stream, /*pretty_print=*/false);
  return stream.GetString().str();
}

class StopTimestampsTest : public ThreadTest {
public:
  void SetUp() override {
    ThreadTest::SetUp();
    ArchSpec arch("x86_64-pc-linux");
    Platform::SetHostPlatform(
        platform_linux::PlatformLinux::CreateInstance(true, &arch));
    m_debugger_sp = Debugger::CreateInstance();
    ASSERT_TRUE(m_debugger_sp);
    m_target_sp = CreateTarget(m_debugger_sp, arch);
    ASSERT_TRUE(m_target_sp);
    ListenerSP listener_sp(Listener::MakeListener("dummy"));
    m_process_sp = std::make_shared<DummyProcess>(m_target_sp, listener_sp);
    m_thread_sp = std::make_shared<DummyThread>(*m_process_sp, 0);
  }

  // A plan that has just completed, as at the end of a step.
  void CompletePlan() {
    m_thread_sp->PushPlan(std::make_shared<DummyThreadPlan>(*m_thread_sp));
    m_thread_sp->PopPlan();
    ASSERT_TRUE(m_thread_sp->GetCompletedPlan());
  }

  DebuggerSP m_debugger_sp;
  TargetSP m_target_sp;
  std::shared_ptr<DummyProcess> m_process_sp;
  std::shared_ptr<DummyThread> m_thread_sp;
};
} // namespace

TEST_F(StopTimestampsTest, ExtendedInfoWithoutOtherInfo) {
  StopInfoSP stop_info_sp =
      StopInfo::CreateStopReasonWithSignal(*m_thread_sp, 2);
  EXPECT_EQ(ToJSON(stop_info_sp->GetExtendedInfoWithStopTimestamps()),
            "<null>");

  stop_info_sp->SetStopTimestamps(
      MakeStopTimestamps("1.000000001", "18446744073709551615"));
  EXPECT_EQ(ToJSON(stop_info_sp->GetExtendedInfoWithStopTimestamps()),
            R"({"conttime":"18446744073709551615","time":"1.000000001"})");
  // Asking again gives the same answer.
  EXPECT_EQ(ToJSON(stop_info_sp->GetExtendedInfoWithStopTimestamps()),
            R"({"conttime":"18446744073709551615","time":"1.000000001"})");

  stop_info_sp->SetStopTimestamps(MakeStopTimestamps(std::nullopt, "7"));
  EXPECT_EQ(ToJSON(stop_info_sp->GetExtendedInfoWithStopTimestamps()),
            R"({"conttime":"7"})");

  // The plugin's own extended info stays empty.
  EXPECT_FALSE(stop_info_sp->GetExtendedInfo());
}

TEST_F(StopTimestampsTest, ExistingEntriesAreKeptAndNotModified) {
  auto info_sp = std::make_shared<StructuredData::Dictionary>();
  info_sp->AddStringItem("issue", "heap-use-after-free");
  info_sp->AddIntegerItem("time", 42);
  StopInfoSP stop_info_sp =
      std::make_shared<ExtendedInfoStopInfo>(*m_thread_sp, info_sp);
  stop_info_sp->SetStopTimestamps(MakeStopTimestamps("1.000000001", "7"));

  // An existing "time" wins, even with another type.
  EXPECT_EQ(ToJSON(stop_info_sp->GetExtendedInfoWithStopTimestamps()),
            R"({"conttime":"7","issue":"heap-use-after-free","time":42})");
  EXPECT_EQ(ToJSON(stop_info_sp->GetExtendedInfo()),
            R"({"issue":"heap-use-after-free","time":42})");
}

TEST_F(StopTimestampsTest, OtherShapesAreReturnedAsTheyAre) {
  auto info_sp = std::make_shared<StructuredData::Array>();
  info_sp->AddStringItem("report");
  StopInfoSP stop_info_sp =
      std::make_shared<ExtendedInfoStopInfo>(*m_thread_sp, info_sp);
  stop_info_sp->SetStopTimestamps(MakeStopTimestamps("1.000000001", "7"));

  EXPECT_EQ(stop_info_sp->GetExtendedInfoWithStopTimestamps(), info_sp);
  EXPECT_EQ(ToJSON(info_sp), R"(["report"])");
}

TEST_F(StopTimestampsTest, ReturnedInfoIsASnapshot) {
  StopInfoSP stop_info_sp =
      StopInfo::CreateStopReasonWithSignal(*m_thread_sp, 2);
  stop_info_sp->SetStopTimestamps(MakeStopTimestamps("1.000000001", "100"));
  StructuredData::ObjectSP before =
      stop_info_sp->GetExtendedInfoWithStopTimestamps();

  stop_info_sp->SetStopTimestamps(MakeStopTimestamps("2.000000002", "200"));
  EXPECT_EQ(ToJSON(before), R"({"conttime":"100","time":"1.000000001"})");
  EXPECT_EQ(ToJSON(stop_info_sp->GetExtendedInfoWithStopTimestamps()),
            R"({"conttime":"200","time":"2.000000002"})");
}

TEST_F(StopTimestampsTest, CompletedStepReportsTheStopThatCompletedIt) {
  // An instruction step: the private stop is a trace, and the public stop
  // info is made from the completed plan on request.
  StopInfoSP trace_sp = StopInfo::CreateStopReasonToTrace(*m_thread_sp);
  trace_sp->SetStopTimestamps(MakeStopTimestamps("1.000000001", "100"));
  m_thread_sp->SetStopInfo(trace_sp);
  CompletePlan();

  StopInfoSP public_sp = m_thread_sp->GetStopInfo();
  ASSERT_TRUE(public_sp);
  EXPECT_EQ(public_sp->GetStopReason(), eStopReasonPlanComplete);
  EXPECT_EQ(ToJSON(public_sp->GetExtendedInfoWithStopTimestamps()),
            R"({"conttime":"100","time":"1.000000001"})");
}

TEST_F(StopTimestampsTest, PublicStopInfoKeepsTheStopsTimestamps) {
  // A step that ends at one of its own breakpoints: the private stop info is
  // replaced by CalculatePublicStopInfo.
  StopInfoSP breakpoint_sp =
      StopInfo::CreateStopReasonWithBreakpointSiteID(*m_thread_sp, 0);
  breakpoint_sp->SetStopTimestamps(MakeStopTimestamps("1.000000001", "100"));
  m_thread_sp->SetStopInfo(breakpoint_sp);
  CompletePlan();

  m_thread_sp->CalculatePublicStopInfo();
  StopInfoSP public_sp = m_thread_sp->GetStopInfo();
  ASSERT_TRUE(public_sp);
  EXPECT_EQ(public_sp->GetStopReason(), eStopReasonPlanComplete);
  EXPECT_EQ(ToJSON(public_sp->GetExtendedInfoWithStopTimestamps()),
            R"({"conttime":"100","time":"1.000000001"})");
}

TEST_F(StopTimestampsTest, CompletedPlanDoesNotTakeAnEarlierStopsTime) {
  StopInfoSP trace_sp = StopInfo::CreateStopReasonToTrace(*m_thread_sp);
  trace_sp->SetStopTimestamps(MakeStopTimestamps("1.000000001", "100"));
  m_thread_sp->SetStopInfo(trace_sp);
  m_process_sp->GetModIDNonConstRef().BumpStopID();
  CompletePlan();

  StopInfoSP public_sp = m_thread_sp->GetStopInfo();
  ASSERT_TRUE(public_sp);
  EXPECT_EQ(public_sp->GetStopReason(), eStopReasonPlanComplete);
  EXPECT_EQ(ToJSON(public_sp->GetExtendedInfoWithStopTimestamps()), "<null>");
}

TEST_F(StopTimestampsTest, KeptStopKeepsItsTimestamps) {
  StopInfoSP breakpoint_sp =
      StopInfo::CreateStopReasonWithBreakpointSiteID(*m_thread_sp, 0);
  breakpoint_sp->SetStopTimestamps(MakeStopTimestamps("1.000000001", "100"));
  m_thread_sp->SetStopInfo(breakpoint_sp);

  // The thread is still at the breakpoint it hit, so it keeps that stop.
  m_process_sp->GetModIDNonConstRef().BumpStopID();
  StopInfoSP kept_sp = m_thread_sp->GetPrivateStopInfo();
  EXPECT_EQ(kept_sp, breakpoint_sp);
  EXPECT_EQ(ToJSON(kept_sp->GetExtendedInfoWithStopTimestamps()),
            R"({"conttime":"100","time":"1.000000001"})");
}
