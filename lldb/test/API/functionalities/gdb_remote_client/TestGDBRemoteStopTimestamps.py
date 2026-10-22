"""
Test that the "time" and "conttime" stop reply fields reach
SBThread::GetStopReasonExtendedInfoAsJSON for the stop they describe, through
each packet that can carry them.
"""

import json

import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
from lldbsuite.test.gdbclientutils import *
from lldbsuite.test.lldbgdbclient import GDBRemoteTestBase


def jstopinfo(*threads):
    return "jstopinfo:%s;" % json.dumps(list(threads)).encode().hex()


def thread_info(tid, time=None, conttime=None):
    """A jstopinfo or jThreadsInfo entry for a thread stopped by SIGINT."""
    info = {"tid": tid, "signal": 2, "reason": "signal"}
    if time is not None:
        info["time"] = time
    if conttime is not None:
        info["conttime"] = conttime
    return info


class StopResponder(MockGDBServerResponder):
    """Replies to the initial "?" with stops[0] and to the n-th resume with
    stops[n]. thread_stops[n] and threads_info[n] are the qThreadStopInfo and
    jThreadsInfo replies while stopped at stops[n]; without them the packet is
    unsupported."""

    def __init__(self, stops, thread_stops=None, threads_info=None):
        super().__init__()
        self.stops = stops
        self.thread_stops = thread_stops
        self.threads_info = threads_info
        self.current = 0

    def qXferRead(self, obj, annex, offset, length):
        if annex == "target.xml":
            return (
                """<?xml version="1.0"?>
                <target version="1.0">
                  <architecture>i386:x86-64</architecture>
                  <feature name="org.gnu.gdb.i386.core">
                    <reg name="rip" bitsize="64" regnum="0" type="code_ptr"
                         group="general"/>
                  </feature>
                </target>""",
                False,
            )
        return None, False

    def haltReason(self):
        return self.stops[self.current]

    def threadStopInfo(self, threadnum):
        if self.thread_stops is None:
            return ""
        return self.thread_stops.get(self.current, {}).get(threadnum, "E01")

    def jThreadsInfo(self):
        if self.threads_info is None:
            return ""
        return escape_binary(json.dumps(self.threads_info.get(self.current, [])))

    def _resume(self):
        self.current += 1
        return self.stops[self.current]

    def cont(self):
        return self._resume()

    def vCont(self, packet):
        return self._resume()

    def other(self, packet):
        if packet == "vCont?":
            return "vCont;c;C;s;S"
        if packet.startswith("vCont;"):
            return self._resume()
        return ""


class TestGDBRemoteStopTimestamps(GDBRemoteTestBase):
    def stop_timestamps(self, thread):
        """The timestamp entries of the thread's stop, or None if the stop has
        no extended info."""
        stream = lldb.SBStream()
        if not thread.GetStopReasonExtendedInfoAsJSON(stream):
            return None
        info = json.loads(stream.GetData())
        return {key: info[key] for key in ("time", "conttime") if key in info}

    def connect_to(self, responder):
        self.server.responder = responder
        target = self.dbg.CreateTarget("")
        if self.TraceOn():
            self.runCmd("log enable gdb-remote packets")
            self.addTearDownHook(lambda: self.runCmd("log disable gdb-remote packets"))
        return self.connect(target)

    def received(self):
        return self.server.responder.packetLog.get_received()

    @skipIfXmlSupportMissing
    def test_text_stop_reply(self):
        process = self.connect_to(
            StopResponder(
                [
                    "T02thread:1;threads:1;"
                    "time:1729500000.000001000;conttime:18446744073709551615;"
                ]
            )
        )
        thread = process.GetThreadByID(1)
        expected = {
            "time": "1729500000.000001000",
            "conttime": "18446744073709551615",
        }
        self.assertEqual(self.stop_timestamps(thread), expected)
        # Asking again gives the same answer.
        self.assertEqual(self.stop_timestamps(thread), expected)
        self.assertStopReason(thread.GetStopReason(), lldb.eStopReasonSignal)
        self.expect(
            "thread info -s",
            substrs=[
                '"time": "1729500000.000001000"',
                '"conttime": "18446744073709551615"',
            ],
            ordered=False,
        )
        # Timestamps are not instrumentation reports.
        backtraces = thread.GetStopReasonExtendedBacktraces(
            lldb.eInstrumentationRuntimeTypeAddressSanitizer
        )
        self.assertEqual(backtraces.GetSize(), 0)
        self.assertNotIn("qThreadStopInfo1", self.received())

    @skipIfXmlSupportMissing
    def test_qThreadStopInfo(self):
        process = self.connect_to(
            StopResponder(
                ["T02thread:1;threads:1,2;time:1.000000001;conttime:100;"],
                thread_stops={
                    0: {2: "T02thread:2;threads:1,2;time:2.000000002;conttime:200;"}
                },
            )
        )
        self.assertEqual(
            self.stop_timestamps(process.GetThreadByID(2)),
            {"time": "2.000000002", "conttime": "200"},
        )
        self.assertPacketLogReceived(["qThreadStopInfo2"])

    @skipIfXmlSupportMissing
    def test_jstopinfo(self):
        process = self.connect_to(
            StopResponder(
                [
                    "T02thread:1;threads:1,2,3,4;time:1.000000001;conttime:100;"
                    + jstopinfo(
                        # The stop reply already described thread 1.
                        thread_info(1, "9.000000009", "900"),
                        thread_info(2, "2.000000002", "200"),
                        # A server that does not send the fields.
                        thread_info(3),
                    )
                ],
                # Would give different values if lldb asked.
                thread_stops={
                    0: {2: "T02thread:2;threads:1,2,3,4;time:8.000000008;conttime:800;"}
                },
            )
        )
        self.assertEqual(
            self.stop_timestamps(process.GetThreadByID(1)),
            {"time": "1.000000001", "conttime": "100"},
        )
        self.assertEqual(
            self.stop_timestamps(process.GetThreadByID(2)),
            {"time": "2.000000002", "conttime": "200"},
        )
        thread_3 = process.GetThreadByID(3)
        self.assertStopReason(thread_3.GetStopReason(), lldb.eStopReasonSignal)
        self.assertIsNone(self.stop_timestamps(thread_3))
        # No stop reason at all.
        thread_4 = process.GetThreadByID(4)
        self.assertStopReason(thread_4.GetStopReason(), lldb.eStopReasonNone)
        self.assertIsNone(self.stop_timestamps(thread_4))
        self.assertNotIn("qThreadStopInfo2", self.received())

    @skipIfXmlSupportMissing
    def test_jThreadsInfo(self):
        # jThreadsInfo is read at the public stop. It describes a thread's stop
        # only if nothing did before: here, thread 2, which was suspended and
        # had no stop reason, is the only thread it is the source for.
        responder = StopResponder(
            [
                "T02thread:1;threads:1,2;time:1.000000001;conttime:100;"
                + jstopinfo(thread_info(1, "1.000000001", "100")),
                "T02thread:1;threads:1,2;time:3.000000003;conttime:300;"
                + jstopinfo(thread_info(1, "3.000000003", "300")),
            ],
            threads_info={
                0: [thread_info(1, "1.000000001", "100")],
                1: [
                    thread_info(1, "3.000000003", "300"),
                    thread_info(2, "4.000000004", "400"),
                ],
            },
        )
        process = self.connect_to(responder)
        thread_2 = process.GetThreadByID(2)
        self.assertStopReason(thread_2.GetStopReason(), lldb.eStopReasonNone)

        thread_2.Suspend()
        self.assertSuccess(process.Continue())
        self.assertEqual(responder.current, 1)
        self.assertStopReason(thread_2.GetStopReason(), lldb.eStopReasonSignal)
        self.assertEqual(
            self.stop_timestamps(thread_2), {"time": "4.000000004", "conttime": "400"}
        )
        self.assertNotIn("qThreadStopInfo2", self.received())

    @skipIfXmlSupportMissing
    def test_later_replies_do_not_change_a_stop(self):
        # The reply that creates a thread's stop decides its timestamps. A
        # later reply about the same stop does not add, replace or remove them.
        responder = StopResponder(
            [
                "T02thread:1;threads:1,2,3;",
                # Thread 1: only a wall time.
                "T02thread:1;threads:1,2,3;time:1.000000001;"
                + jstopinfo(
                    thread_info(1, "1.000000001", "100"),
                    thread_info(2, "2.000000002", "200"),
                    thread_info(3, "3.000000003", "300"),
                ),
            ],
            # Read at the public stop after the resume.
            threads_info={
                0: [],
                1: [
                    # Adds a field.
                    thread_info(1, "1.000000001", "100"),
                    # Conflicts.
                    thread_info(2, "7.000000007", "700"),
                    # Omits both fields.
                    thread_info(3),
                ],
            },
        )
        process = self.connect_to(responder)
        self.assertSuccess(process.Continue())
        self.assertEqual(responder.current, 1)
        # Only requested at a public stop, which needs a resume.
        self.assertPacketLogReceived(["jThreadsInfo"])
        self.assertEqual(
            self.stop_timestamps(process.GetThreadByID(1)), {"time": "1.000000001"}
        )
        self.assertEqual(
            self.stop_timestamps(process.GetThreadByID(2)),
            {"time": "2.000000002", "conttime": "200"},
        )
        self.assertEqual(
            self.stop_timestamps(process.GetThreadByID(3)),
            {"time": "3.000000003", "conttime": "300"},
        )

    @skipIfXmlSupportMissing
    def test_malformed_values_are_ignored_one_by_one(self):
        packets = {
            # Each field is checked on its own.
            1: "time:1729500000.000001000;conttime:abc;",
            2: "time:1729500000.5;conttime:42;",
            # Out of range, signed, or empty: neither is kept.
            3: "time:-1.000000000;conttime:99999999999999999999;",
            4: "time:+1.000000000;conttime:;",
            5: "time:1.0000000001;conttime:0x10;",
            # The last valid occurrence wins.
            6: "conttime:5;conttime:bogus;conttime:7;time:;",
        }
        stops = {
            tid: "T02thread:%x;threads:1,2,3,4,5,6;%s" % (tid, fields)
            for tid, fields in packets.items()
        }
        process = self.connect_to(StopResponder([stops[1]], thread_stops={0: stops}))
        expected = {
            1: {"time": "1729500000.000001000"},
            2: {"conttime": "42"},
            3: None,
            4: None,
            5: None,
            6: {"conttime": "7"},
        }
        for tid, timestamps in expected.items():
            thread = process.GetThreadByID(tid)
            # The stop itself is unaffected.
            self.assertStopReason(thread.GetStopReason(), lldb.eStopReasonSignal)
            self.assertEqual(self.stop_timestamps(thread), timestamps, "tid %d" % tid)

    @skipIfXmlSupportMissing
    def test_json_values_must_be_strings(self):
        process = self.connect_to(
            StopResponder(
                [
                    "T02thread:1;threads:1,2;"
                    + jstopinfo(thread_info(1), thread_info(2, 1.5, 200))
                ]
            )
        )
        thread = process.GetThreadByID(2)
        self.assertStopReason(thread.GetStopReason(), lldb.eStopReasonSignal)
        self.assertIsNone(self.stop_timestamps(thread))

    @skipIfXmlSupportMissing
    def test_new_stops_and_kept_stops(self):
        responder = StopResponder(
            [
                # Both threads stop, each with its own time.
                "T02thread:1;threads:1,2;time:1.000000001;conttime:100;"
                + jstopinfo(
                    thread_info(1, "1.000000001", "100"),
                    thread_info(2, "2.000000002", "200"),
                ),
                # Thread 2 was suspended. Even if the stub reports it, it keeps
                # the stop it had, and that stop keeps its time.
                "T02thread:2;threads:1,2;time:4.000000004;conttime:400;"
                + jstopinfo(
                    thread_info(1, "3.000000003", "300"),
                    thread_info(2, "4.000000004", "400"),
                ),
                # A new stop without times does not reuse the old ones.
                "T02thread:1;threads:1,2;" + jstopinfo(thread_info(1)),
            ],
            threads_info={
                0: [],
                1: [
                    thread_info(1, "6.000000006", "600"),
                    thread_info(2, "5.000000005", "500"),
                ],
                2: [],
            },
        )
        process = self.connect_to(responder)
        thread_1 = process.GetThreadByID(1)
        thread_2 = process.GetThreadByID(2)
        self.assertEqual(
            self.stop_timestamps(thread_1), {"time": "1.000000001", "conttime": "100"}
        )
        self.assertEqual(
            self.stop_timestamps(thread_2), {"time": "2.000000002", "conttime": "200"}
        )

        thread_2.Suspend()
        self.assertSuccess(process.Continue())
        self.assertEqual(responder.current, 1)
        self.assertEqual(
            self.stop_timestamps(thread_1), {"time": "3.000000003", "conttime": "300"}
        )
        self.assertStopReason(thread_2.GetStopReason(), lldb.eStopReasonSignal)
        self.assertEqual(
            self.stop_timestamps(thread_2), {"time": "2.000000002", "conttime": "200"}
        )

        thread_2.Resume()
        self.assertSuccess(process.Continue())
        self.assertEqual(responder.current, 2)
        self.assertStopReason(thread_1.GetStopReason(), lldb.eStopReasonSignal)
        self.assertIsNone(self.stop_timestamps(thread_1))
        self.assertIsNone(self.stop_timestamps(thread_2))
