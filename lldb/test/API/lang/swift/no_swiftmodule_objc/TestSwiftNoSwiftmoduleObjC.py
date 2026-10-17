"""
Test debugging a binary built without any .swiftmodule, where the Objective-C
types have to come from DWARF and the Objective-C runtime.
"""

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
import lldbsuite.test.lldbutil as lldbutil


class TestSwiftNoSwiftmoduleObjC(TestBase):
    NO_DEBUG_INFO_TESTCASE = True

    @requireNotEmbeddedSwift
    @skipUnlessDarwin
    @swiftTest
    def test(self):
        self.build()
        lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )
        self.expect(
            "frame variable -d run",
            patterns=[
                r"\(size_t\) ctype = 1024",
                # Both of these are tagged pointers to the Objective-C runtime,
                # which is what resolves them.
                # FIXME: object should come out as (ObjCClass) printed through
                # its debugDescription, "Hello from Objective-C!".
                r"\(.*NS.*Number\) object = 0x[0-9a-f]+ Int32\(1234\)",
                r"\(.*NS.*Number\) inlined = 0x[0-9a-f]+ Int64\(42\)",
                r"\(CMYK\) enumerator = \.yellow",
                r"\(FourColors\) typedef = \.cyan",
            ],
        )
        # A swift_newtype only stays visible as the Clang typedef it came from
        # while dynamic type resolution is off; resolving it yields the Swift
        # type standing in for it.
        self.expect(
            "frame variable -d no-dynamic-values renamed",
            patterns=[
                r'\(OBJCSTUFF_MyString\) renamed = 0x[0-9a-f]+ "with swift_name"'
            ],
        )
        self.expect(
            "target variable globalFloat",
            patterns=[r"\(const MyFloat\) globalFloat = 3\.14"],
        )

    # FIXME: `frame variable -O object` fails creating the expression context.
