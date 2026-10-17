"""
Test debugging a module built without a .swiftmodule that imports types from a
module which has one. Reconstructing those types used to crash (rdar://60734897).
"""

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
import lldbsuite.test.lldbutil as lldbutil


class TestSwiftNoSwiftmodule(TestBase):
    NO_DEBUG_INFO_TESTCASE = True

    @requireNotEmbeddedSwift
    @swiftTest
    def test(self):
        self.build()
        lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )
        self.expect("frame variable", substrs=["(NoSwiftmoduleHelper.S2) x = {}"])
        self.runCmd("up")
        self.expect(
            "frame variable",
            substrs=["(Int) t = 23", "(NoSwiftmoduleHelper.S2) strct2 = {}"],
        )
