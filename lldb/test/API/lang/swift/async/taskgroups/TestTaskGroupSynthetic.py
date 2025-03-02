import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class TestCase(TestBase):

    @swiftTest
    def test(self):
        """Print a TaskGroup and verify its children."""
        self.build()
        lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )
        self.expect(
            "v group",
            substrs=[
                "[0] = {",
                "isGroupChildTask = true",
                "[1] = {",
                "isGroupChildTask = true",
                "[2] = {",
                "isGroupChildTask = true",
            ],
        )
