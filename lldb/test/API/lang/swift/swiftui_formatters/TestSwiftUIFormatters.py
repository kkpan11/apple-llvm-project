import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class TestCase(TestBase):

    def setUp(self):
        super().setUp()
        # In some environments (specifically seen in PR CI), TypeSystemSwiftTypeRef
        # fails to retrieve the entirety of SwiftUI.State<T>. In those environments,
        # fallback to the compiler is needed, and validation needs to be disabled.
        self.runCmd("settings set symbols.swift-validate-typesystem false")
        self.runCmd("settings set symbols.swift-typesystem-compiler-fallback true")

    @skipUnlessDarwin
    @swiftTest
    def test_before(self):
        self.build()
        _, _, self.thread, _ = lldbutil.run_to_source_breakpoint(
            self, "break before", lldb.SBFileSpec("main.swift")
        )
        self._do_test("view._count", 41, is_graph_update=False)

    @skipUnlessDarwin
    @swiftTest
    def test_body(self):
        self.build()
        _, _, self.thread, _ = lldbutil.run_to_source_breakpoint(
            self, "break body", lldb.SBFileSpec("main.swift")
        )
        self._do_test("self._count", 41, is_graph_update=True)

    @skipUnlessDarwin
    @swiftTest
    def disabled_test_after(self):
        self.build()
        _, _, self.thread, _ = lldbutil.run_to_source_breakpoint(
            self, "break after", lldb.SBFileSpec("main.swift")
        )
        self._do_test("self._count", 15, is_graph_update=False)

    @skipUnlessDarwin
    @swiftTest
    def test_final(self):
        self.build()
        _, _, self.thread, _ = lldbutil.run_to_source_breakpoint(
            self, "break final", lldb.SBFileSpec("main.swift")
        )
        self._do_test("self._count", 23, is_graph_update=False)

    def _do_test(self, var_name: str, value: int, *, is_graph_update: bool):
        symbol = "AG::Graph::UpdateStack::update()"
        if is_graph_update:
            self.assertIn(symbol, (f.name for f in self.thread))
        else:
            self.assertNotIn(symbol, (f.name for f in self.thread))

        frame = self.thread.selected_frame
        count = frame.var(var_name)
        self.assertEqual(count.GetNumChildren(), 1)
        self.assertEqual(count.member["wrappedValue"].unsigned, value)
        self.assertEqual(count.summary, str(value))
