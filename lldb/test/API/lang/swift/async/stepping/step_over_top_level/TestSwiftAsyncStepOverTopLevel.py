import lldb
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbtest as lldbtest
import lldbsuite.test.lldbutil as lldbutil


@skipIfAsan  # rdar://138777205
class TestCase(lldbtest.TestBase):

    def check_is_in_line(self, thread, linenum):
        frame = thread.frames[0]
        line_entry = frame.GetLineEntry()
        self.assertEqual(linenum, line_entry.GetLine())

    @skipEmbeddedSwift
    @swiftTest
    @skipIf(oslist=["windows"])
    def test(self):
        """Test async step-over in the funclets of async top-level code."""
        self.build()

        source_file = lldb.SBFileSpec("main.swift")
        target, process, thread, bkpt = lldbutil.run_to_source_breakpoint(
            self, "BREAK HERE", source_file
        )
        bkpt.SetEnabled(False)

        line_offset = 6  # Line where print(x) is.
        expected_line_nums = [0]  # print(x)
        loop = [1, 2, 3, 4]
        expected_line_nums += loop + loop + [1]  # loop header
        expected_line_nums += [5, 6]  # if line + if block

        for expected_line_num in expected_line_nums:
            thread.StepOver()
            stop_reason = thread.GetStopReason()
            self.assertStopReason(stop_reason, lldb.eStopReasonPlanComplete)
            self.check_is_in_line(thread, expected_line_num + line_offset)
            function = thread.frames[0].GetFunction()
            self.assertIn("async_Main", function.GetMangledName())
