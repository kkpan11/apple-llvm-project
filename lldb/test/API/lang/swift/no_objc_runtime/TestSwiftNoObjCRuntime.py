import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil


class TestSwiftNoObjCRuntime(TestBase):
    NO_DEBUG_INFO_TESTCASE = True

    # Objective-C classes are only visible through the ClangImporter.
    @skipIf(swift_module_importer="noclang")
    @skipEmbeddedSwift
    @skipUnlessObjCInterop
    @swiftTest
    def test(self):
        """Test Swift expressions that use Objective-C classes when LLDB
        has no Objective-C runtime for the process"""
        self.build()
        self.runCmd("plugin disable language-runtime.apple-objc-v2")
        self.addTearDownHook(
            lambda: self.runCmd("plugin enable language-runtime.apple-objc-v2")
        )
        lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )
        self.expect("expr NSObject.self", substrs=["NSObject.Type"])
        self.expect("expr f.i", substrs=["Int", "23"])
