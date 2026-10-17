import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil


class TestSwiftClangTypeInfoRehash(TestBase):
    """TypeInfos that LLDB builds for Clang types must stay valid while the
    cache holding them grows."""

    NO_DEBUG_INFO_TESTCASE = True

    def check_first(self, frame):
        first = frame.FindVariable("first")
        self.assertTrue(first.IsValid())
        self.assertEqual(first.GetNumChildren(), 2)
        val = first.GetChildAtIndex(0).GetChildMemberWithName("val")
        self.assertEqual(val.GetChildAtIndex(0).GetValueAsUnsigned(), 1)
        self.assertEqual(first.GetChildAtIndex(1).GetValueAsUnsigned(), 42)

    @swiftClangImporterTest
    @skipEmbeddedSwiftOnWindows
    def test(self):
        self.build()
        self.runCmd("settings set symbols.swift-enable-ast-context false")
        target, process, thread, bkpt = lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )
        frame = thread.frames[0]

        # Cache the TypeInfo for Foo.
        self.check_first(frame)

        # Building the TypeInfo for Big inserts enough Clang types to
        # grow the cache.
        big = frame.FindVariable("big")
        self.assertTrue(big.IsValid())
        self.assertEqual(big.GetNumChildren(), 2)
        payload = big.GetChildAtIndex(0)
        self.assertEqual(payload.GetNumChildren(), 60)
        r0 = payload.GetChildMemberWithName("r0").GetChildMemberWithName("f")
        self.assertEqual(r0.GetValueAsSigned(), 100)
        r59 = payload.GetChildMemberWithName("r59").GetChildMemberWithName("f")
        self.assertEqual(r59.GetValueAsSigned(), 159)
        self.assertEqual(big.GetChildAtIndex(1).GetValueAsUnsigned(), 23)

        # The cached TypeInfo for Foo must have survived.
        self.check_first(frame)
