import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import TestBase
import lldbsuite.test.lldbutil as lldbutil


class TestCase(TestBase):

    def test_backtrace_selected_task(self):
        self.build()
        lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )
        self.runCmd("language swift task select task")
        self.expect(
            "thread backtrace",
            substrs=[
                ".sleep(",
                "`second() at main.swift:6:",
                "`first() at main.swift:2:",
                "`closure #1 in static Main.main() at main.swift:12:",
            ],
        )

    def test_navigate_selected_task_stack(self):
        self.build()
        # target, process, thread, bkpt
        _, process, _, _ = lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )
        self.runCmd("language swift task select task")

        self.expect(
            "thread list",
            substrs=[
                "* thread #4294967295: tid = ",
                "libswift_Concurrency.dylib",
                "._sleep(",
            ],
        )

        frame_index = 0
        for frame in process.selected_thread:
            if "`second()" in str(frame):
                frame_index = frame.idx
        self.assertNotEqual(frame_index, -1)

        self.expect(
            f"frame select {frame_index}",
            substrs=[
                f"frame #{frame_index}:",
                "`second() at main.swift:6:",
                "   5   \tfunc second() async {",
                "-> 6   \t    try? await Task.sleep(for: .seconds(10))",
            ],
        )

        self.expect(
            "up",
            substrs=[
                f"frame #{frame_index + 1}:",
                "`first() at main.swift:2:",
                "   1   \tfunc first() async {",
                "-> 2   \t    await second()",
            ],
        )

        self.expect(
            "up",
            substrs=[
                f"frame #{frame_index + 2}:",
                "`closure #1 in static Main.main() at main.swift:12:",
                "-> 12  \t            await first()",
            ],
        )
