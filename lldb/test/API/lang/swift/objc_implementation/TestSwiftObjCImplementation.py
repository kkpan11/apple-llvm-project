import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil


class TestCase(TestBase):
    def test(self):
        self.build()
        lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )
        self.expect(
            "frame var g",
            substrs=[
                "integer = 15",
                "boolean = true",
                "object = some",
                'stringObject = "Joker"',
            ],
        )
        # Swift types that are not representable in ObjC (bridged types such as
        # String) are not currently listed in the children. rdar://154046212
        self.expect("frame var g", matching=False, substrs=['string = "Ace"'])
