"""
A member function pointer constant is a { i64, i64 } aggregate whose first
element is a signed function pointer.
"""

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil
from lldbsuite.test.lldbarm64e import Arm64eTestBase


class TestPtrAuthMemberFunctionPointer(Arm64eTestBase):
    NO_DEBUG_INFO_TESTCASE = True
    SHARED_BUILD_TESTCASE = False

    def test(self):
        self.build()
        lldbutil.run_to_source_breakpoint(
            self, "// break here", lldb.SBFileSpec("main.cpp", False)
        )

        frame = self.frame()
        mp = frame.FindVariable("mp")
        expr_mp = frame.EvaluateExpression("&S::foo")
        self.assertSuccess(expr_mp.GetError())
        self.assertEqual(expr_mp.GetValue(), mp.GetValue())

        self.expect_expr(
            "int (S::*mp2)() = &S::foo; (s.*mp2)()",
            result_type="int",
            result_value="7",
        )
