import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class TestCase(TestBase):

    @skipUnlessDarwin
    @swiftTest
    def test(self):
        self.build()
        _, _, thread, _ = lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )

        frame = thread.selected_frame

        int_state = frame.var("intState")
        self.assertEqual(int_state.GetNumChildren(), 1)
        self.assertEqual(int_state.member["wrappedValue"].unsigned, 42)
        self.assertIn(int_state.summary, "42")

        str_state = frame.var("strState")
        self.assertEqual(str_state.summary, '"hello"')
