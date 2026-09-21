import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil


class TestSwiftRuntimeInitialization(TestBase):
    @requireNotEmbeddedSwift
    @swiftTest
    def test_dynamic_type_with_exception_breakpoint(self):
        """
        SwiftLanguageRuntime tells the Swift runtime library which ABI version
        the debugger speaks through a global flag. That flag has to be
        initialized against the fully loaded process image, so anything that
        pulls in another module -- setting the Swift exception breakpoint
        loads the Objective-C module -- must not be able to race ahead of it.
        Otherwise the flag holds the wrong value and dynamic type resolution
        stops working.
        """
        self.build()
        target = lldbutil.run_to_breakpoint_make_target(self)

        breakpoint = target.BreakpointCreateBySourceRegex(
            "break here", lldb.SBFileSpec("main.swift")
        )

        # Same defaults as `breakpoint set -E swift`. This has to happen before
        # the process is launched to exercise the initialization order above.
        exception_breakpoint = target.BreakpointCreateForException(
            lldb.eLanguageTypeSwift, False, True
        )
        self.assertTrue(exception_breakpoint.IsValid(), VALID_BREAKPOINT)

        lldbutil.run_to_breakpoint_do_run(self, target, breakpoint)

        # p2 is statically typed as the protocol P, so printing it as a.C is
        # what proves the dynamic type was resolved.
        self.expect("target variable p2", substrs=["(a.C) p2", "p = 42"])
