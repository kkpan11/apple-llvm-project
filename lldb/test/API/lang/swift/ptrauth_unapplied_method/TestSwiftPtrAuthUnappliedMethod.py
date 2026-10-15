import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil
from lldbsuite.test.lldbarm64e import Arm64eTestBase


class TestSwiftPtrAuthUnappliedMethod(Arm64eTestBase):
    NO_DEBUG_INFO_TESTCASE = True
    SHARED_BUILD_TESTCASE = False

    @requireNotEmbeddedSwift  # the embedded harness appends its own non-arm64e -target
    @swiftTest
    def test(self):
        """An unapplied method reference produces a closure constant
        { ptr @thunk.ptrauth, ptr undef } that needs pointer signing fixups."""
        self.build()
        lldbutil.run_to_source_breakpoint(
            self, "// break here", lldb.SBFileSpec("main.swift")
        )

        self.expect("expression c.bar")
        self.expect("expression c.foo")
        # Calling the closure authenticates the signed thunk pointer.
        self.expect_expr("let f = c.bar; f()", result_type="Int", result_value="23")
        self.expect_expr("let g = c.foo; g()", result_type="Int", result_value="42")
