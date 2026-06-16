"""
Test that context-free po doesn't generate malformed generic code that requires
an internal expression evaluation retry.
"""

import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil


class TestCase(TestBase):
    @swiftTest
    @skipEmbeddedSwift
    def test(self):
        self.build()
        lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )
        log = self.getBuildArtifact("expr.log")
        self.runCmd(f"log enable lldb expr -f {log}")
        self.expect("po value", substrs=["instance of C"])
        self.filecheck_log(log, __file__)
        # CHECK-NOT: $__lldb_user_expr<>
        # CHECK: stringForPrintObject(_:mangledTypeName:) succeeded
