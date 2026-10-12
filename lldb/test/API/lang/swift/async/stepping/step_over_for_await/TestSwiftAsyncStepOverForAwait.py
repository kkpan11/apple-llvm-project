import lldb
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbtest as lldbtest
import lldbsuite.test.lldbutil as lldbutil


@skipIfAsan  # rdar://138777205
class TestCase(lldbtest.TestBase):

    def step_over_loop_header(self, break_text, body_text):
        self.build()
        source_file = lldb.SBFileSpec("main.swift")
        target, process, thread, bkpt = lldbutil.run_to_source_breakpoint(
            self, break_text, source_file
        )
        bkpt.SetEnabled(False)

        thread.StepOver()
        self.assertStopReason(thread.GetStopReason(), lldb.eStopReasonPlanComplete)
        frame = thread.frames[0]
        self.assertEqual(
            frame.GetLineEntry().GetLine(),
            lldbtest.line_number("main.swift", body_text),
        )
        return frame

    @skipEmbeddedSwift
    @swiftTest
    @skipIf(oslist=["windows"])
    def test_top_level_code(self):
        """Test stepping over a 'for await' header in async top-level code."""
        frame = self.step_over_loop_header(
            "break in top level code", "top level loop body"
        )
        self.assertIn("async_Main", frame.GetFunction().GetMangledName())

    @skipEmbeddedSwift
    @swiftTest
    @skipIf(oslist=["windows"])
    def test_function(self):
        """Test stepping over a 'for await' header in an async function."""
        frame = self.step_over_loop_header("break in function", "function loop body")
        self.assertIn("$s1a3fooyyYaF", frame.GetFunction().GetMangledName())

    @skipEmbeddedSwift
    @swiftTest
    @skipIf(oslist=["windows"])
    def test_unwind_at_q_funclet_breakpoint(self):
        """Test unwinding where a breakpoint by name stops in a Q funclet."""
        self.build()
        _, _, thread, _ = lldbutil.run_to_name_breakpoint(
            self, self.swiftMangledName("$s1a3fooyyYaFTQ1_")
        )
        frames = thread.frames
        self.assertGreater(len(frames), 1, f"Invalid backtrace: {frames}")
        self.assertIn("$s1a3fooyyYaF", frames[0].GetFunction().GetMangledName())
        self.assertIn("async_Main", frames[1].GetFunction().GetMangledName())
