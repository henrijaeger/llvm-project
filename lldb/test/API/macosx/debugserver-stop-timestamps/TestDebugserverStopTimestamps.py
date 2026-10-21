"""
Test that debugserver reports when it received each thread's stop, and that
lldb keeps that time with the stop it belongs to.
"""

import ctypes
import json
import re
import time

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil

import side_effect

_libsystem = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
_libsystem.mach_continuous_time.restype = ctypes.c_uint64


def host_clocks():
    return time.time_ns(), _libsystem.mach_continuous_time()


class TestDebugserverStopTimestamps(TestBase):
    NO_DEBUG_INFO_TESTCASE = True

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

    @skipUnlessDarwin
    @skipIfOutOfTreeDebugserver
    @skipIfRemote
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

        before = host_clocks()
        process = target.LaunchSimple(None, None, self.get_process_working_directory())
        after = host_clocks()
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
            before = host_clocks()
            step()
            after = host_clocks()
            self.assertStopReason(thread.GetStopReason(), lldb.eStopReasonPlanComplete)
            self.assertEqual(thread.GetFrameAtIndex(0).GetFunctionName(), function)
            timestamp = self.stop_timestamp(thread)
            self.assertBetween(timestamp, before, after)
            self.assertGreater(timestamp[1], previous[1])
            # Asking again gives the same answer.
            self.assertEqual(self.stop_timestamp(thread), timestamp)
            previous = timestamp

    @skipUnlessDarwin
    @skipIfOutOfTreeDebugserver
    @skipIfRemote
    def test_each_thread_reports_its_own_exception(self):
        self.build()
        target = self.createTestTarget()
        target.BreakpointCreateByName("worker_hit")
        before = host_clocks()
        process = target.LaunchSimple(None, None, self.get_process_working_directory())

        hits = 0
        stops_with_several_hits = 0
        last_seen = {}
        while process.GetState() == lldb.eStateStopped:
            after = host_clocks()
            new = []
            for thread in lldbutil.get_stopped_threads(
                process, lldb.eStopReasonBreakpoint
            ):
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
                # Each thread has the time of its own exception message.
                stops_with_several_hits += 1
                conttimes = [timestamp[1] for timestamp in new]
                self.assertEqual(len(set(conttimes)), len(conttimes))
            hits += len(new)
            before = host_clocks()
            process.Continue()

        self.assertEqual(hits, 40)
        if self.TraceOn():
            print("stops with several hits:", stops_with_several_hits)

    @skipUnlessDarwin
    @skipIfOutOfTreeDebugserver
    @skipIfRemote
    def test_suspended_thread_keeps_its_stop(self):
        self.build()
        target = self.createTestTarget()
        bkpt_a = target.BreakpointCreateByName("stop_a")
        bkpt_b = target.BreakpointCreateByName("stop_b")
        process = target.LaunchSimple(
            ["suspend"], None, self.get_process_working_directory()
        )
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
