"""
Test that debugserver reports when it received each thread's stop, and that
lldb keeps that time with the stop it belongs to.
"""

import ctypes
import json
import time

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil

import side_effect


@skipUnlessDarwin
@skipIfOutOfTreeDebugserver
@skipIfRemote
class TestDebugserverStopTimestamps(TestBase):
    NO_DEBUG_INFO_TESTCASE = True

    def host_clocks(self):
        """(wall time in ns, continuous time) on this host. The target runs on
        this host too, so its stops lie between two such readings."""
        if not hasattr(self, "_mach_continuous_time"):
            libsystem = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
            try:
                self._mach_continuous_time = libsystem.mach_continuous_time
            except AttributeError:
                self.skipTest("mach_continuous_time() is not available")
            self._mach_continuous_time.restype = ctypes.c_uint64
        return time.time_ns(), self._mach_continuous_time()

    def stop_timestamp(self, thread):
        """Returns (wall time in ns, continuous time) for the thread's stop."""
        stream = lldb.SBStream()
        self.assertTrue(
            thread.GetStopReasonExtendedInfoAsJSON(stream),
            "no extended info for thread %d" % thread.GetIndexID(),
        )
        return self.parse(json.loads(stream.GetData()))

    def parse(self, info):
        self.assertRegex(info.get("time", ""), r"^\d+\.\d{9}$")
        self.assertRegex(info.get("conttime", ""), r"^\d+$")
        seconds, fraction = info["time"].split(".")
        return int(seconds) * 10**9 + int(fraction), int(info["conttime"])

    def assertBetween(self, timestamp, before, after):
        self.assertLessEqual(before[0], timestamp[0])
        self.assertLessEqual(timestamp[0], after[0])
        self.assertLessEqual(before[1], timestamp[1])
        self.assertLessEqual(timestamp[1], after[1])

    def assertNoTimestamp(self, thread):
        stream = lldb.SBStream()
        self.assertFalse(thread.GetStopReasonExtendedInfoAsJSON(stream))

    def launch(self, target, args=None, flags=0):
        launch_info = target.GetLaunchInfo()
        if args:
            launch_info.SetArguments(args, True)
        launch_info.SetWorkingDirectory(self.get_process_working_directory())
        launch_info.SetLaunchFlags(flags)
        error = lldb.SBError()
        process = target.Launch(launch_info, error)
        self.assertSuccess(error)
        return process

    def test_breakpoint_callback_and_stepping(self):
        self.build()
        target = self.createTestTarget()
        bkpt = target.BreakpointCreateBySourceRegex(
            "break here", lldb.SBFileSpec("main.c")
        )
        side_effect.info = None
        bkpt.SetScriptCallbackBody(
            "import json, side_effect\n"
            "stream = lldb.SBStream()\n"
            "frame.GetThread().GetStopReasonExtendedInfoAsJSON(stream)\n"
            "side_effect.info = json.loads(stream.GetData())"
        )

        before = self.host_clocks()
        process = self.launch(target)
        after = self.host_clocks()
        thread = lldbutil.get_one_thread_stopped_at_breakpoint(process, bkpt)
        self.assertIsNotNone(thread)

        # The callback sees the same time as the public API.
        self.assertIsNotNone(side_effect.info)
        previous = self.parse(side_effect.info)
        self.assertEqual(self.stop_timestamp(thread), previous)
        self.assertBetween(previous, before, after)

        # Into step_target, over its first line, back out to main, and one
        # instruction in main.
        steps = [
            (thread.StepInto, "step_target"),
            (thread.StepOver, "step_target"),
            (thread.StepOut, "main"),
            (lambda: thread.StepInstruction(False), "main"),
        ]
        for step, function in steps:
            before = self.host_clocks()
            step()
            after = self.host_clocks()
            self.assertStopReason(thread.GetStopReason(), lldb.eStopReasonPlanComplete)
            self.assertEqual(thread.GetFrameAtIndex(0).GetFunctionName(), function)
            timestamp = self.stop_timestamp(thread)
            self.assertBetween(timestamp, before, after)
            self.assertGreater(timestamp[1], previous[1])
            # Asking again gives the same answer.
            self.assertEqual(self.stop_timestamp(thread), timestamp)
            previous = timestamp

    def test_each_thread_reports_its_own_exception(self):
        """Stress test. Whether two workers stop in the same batch of
        exceptions is up to the scheduler, so this checks every stop it sees
        and reports how many had several new stops. The deterministic tests
        are MachExceptionTest's."""
        self.build()
        target = self.createTestTarget()
        target.BreakpointCreateByName("worker_hit")
        before = self.host_clocks()
        process = self.launch(target)

        hits = 0
        stops_with_several_hits = 0
        last_seen = {}
        while process.GetState() == lldb.eStateStopped:
            after = self.host_clocks()
            new = []
            for thread in process:
                if thread.GetStopReason() != lldb.eStopReasonBreakpoint:
                    # Nothing happened to this thread, so it has no time.
                    if thread.GetStopReason() == lldb.eStopReasonNone:
                        self.assertNoTimestamp(thread)
                    continue
                timestamp = self.stop_timestamp(thread)
                tid = thread.GetThreadID()
                # lldb may show a thread that has not run since its last stop
                # with that stop, and then the stop's time comes with it.
                if timestamp == last_seen.get(tid):
                    continue
                self.assertBetween(timestamp, before, after)
                last_seen[tid] = timestamp
                new.append(timestamp)
            if len(new) > 1:
                stops_with_several_hits += 1
                conttimes = [timestamp[1] for timestamp in new]
                self.assertEqual(len(set(conttimes)), len(conttimes))
            hits += len(new)
            before = self.host_clocks()
            process.Continue()

        self.assertEqual(hits, 40)
        self.trace("stops with several hits: %d" % stops_with_several_hits)

    def test_suspended_thread_keeps_its_stop(self):
        self.build()
        target = self.createTestTarget()
        bkpt_a = target.BreakpointCreateByName("stop_a")
        bkpt_b = target.BreakpointCreateByName("stop_b")
        process = self.launch(target, ["suspend"])
        (thread_a,) = lldbutil.get_threads_stopped_at_breakpoint(process, bkpt_a)
        timestamp_a = self.stop_timestamp(thread_a)

        # Let thread B reach its breakpoint while thread A stays where it is.
        go_b = target.FindFirstGlobalVariable("go_b")
        self.assertTrue(go_b.SetValueFromCString("1"))
        thread_a.Suspend()
        process.Continue()

        (thread_b,) = lldbutil.get_threads_stopped_at_breakpoint(process, bkpt_b)
        self.assertNotEqual(thread_b.GetThreadID(), thread_a.GetThreadID())
        timestamp_b = self.stop_timestamp(thread_b)
        self.assertGreater(timestamp_b[1], timestamp_a[1])

        # Thread A did not run, so it keeps its stop and the time of that stop.
        self.assertStopReason(thread_a.GetStopReason(), lldb.eStopReasonBreakpoint)
        self.assertEqual(self.stop_timestamp(thread_a), timestamp_a)

    def test_launch_and_relaunch(self):
        self.build()
        target = self.createTestTarget()
        bkpt = target.BreakpointCreateBySourceRegex(
            "break here", lldb.SBFileSpec("main.c")
        )
        previous = (0, 0)
        for _ in range(2):
            # debugserver stops a new process with a SIGSTOP exception.
            before = self.host_clocks()
            process = self.launch(target, flags=lldb.eLaunchFlagStopAtEntry)
            after = self.host_clocks()
            thread = process.GetSelectedThread()
            self.assertStopReason(thread.GetStopReason(), lldb.eStopReasonSignal)
            entry = self.stop_timestamp(thread)
            self.assertBetween(entry, before, after)
            self.assertGreater(entry[1], previous[1])

            before = self.host_clocks()
            process.Continue()
            after = self.host_clocks()
            (thread,) = lldbutil.get_threads_stopped_at_breakpoint(process, bkpt)
            previous = self.stop_timestamp(thread)
            self.assertBetween(previous, before, after)
            self.assertSuccess(process.Kill())

    def test_exec(self):
        self.build()
        target = self.createTestTarget()
        bkpt = target.BreakpointCreateByName("after_exec")
        before = self.host_clocks()
        process = self.launch(target, ["exec"])
        after = self.host_clocks()

        thread = process.GetSelectedThread()
        self.assertStopReason(thread.GetStopReason(), lldb.eStopReasonExec)
        exec_stop = self.stop_timestamp(thread)
        self.assertBetween(exec_stop, before, after)

        before = self.host_clocks()
        process.Continue()
        after = self.host_clocks()
        (thread,) = lldbutil.get_threads_stopped_at_breakpoint(process, bkpt)
        timestamp = self.stop_timestamp(thread)
        self.assertBetween(timestamp, before, after)
        self.assertGreater(timestamp[1], exec_stop[1])

    def test_attach_detach_and_reattach(self):
        self.build()
        exe = self.getBuildArtifact("a.out")
        popen = self.spawnSubprocess(exe, ["wait"])
        target = self.dbg.CreateTarget(exe)
        previous = (0, 0)
        for _ in range(2):
            # debugserver stops the process it attaches to with SIGSTOP.
            error = lldb.SBError()
            before = self.host_clocks()
            process = target.AttachToProcessWithID(lldb.SBListener(), popen.pid, error)
            after = self.host_clocks()
            self.assertSuccess(error)
            thread = process.GetSelectedThread()
            self.assertStopReason(thread.GetStopReason(), lldb.eStopReasonSignal)
            timestamp = self.stop_timestamp(thread)
            self.assertBetween(timestamp, before, after)
            self.assertGreater(timestamp[1], previous[1])
            previous = timestamp
            self.assertSuccess(process.Detach())
