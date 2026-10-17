import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil


class TestSwiftDeserializationFailure(TestBase):
    def prepare(self):
        import shutil
        copied_source = self.getBuildArtifact("main.swift")
        shutil.copyfile(os.path.join("Inputs", "main.swift"), copied_source)
        self.build()
        os.unlink(copied_source)
        os.unlink(self.getBuildArtifact("a.swiftmodule"))

    def run_tests(self, target, process):
        static_bkpt = target.BreakpointCreateByName('staticTypes')
        dynamic_bkpt = target.BreakpointCreateByName('dynamicTypes')
        generic_bkpt = target.BreakpointCreateByName('genericTypes')
        lldbutil.continue_to_breakpoint(process, static_bkpt)
        self.expect("fr var i", substrs=["23"])
        self.expect("fr var s", substrs=["(String)", "world"])

        # We should not be able to resolve the types defined in the module.
        lldbutil.continue_to_breakpoint(process, dynamic_bkpt)
        # FIXME: Resurface this error!
        self.expect("fr var c", substrs=[""]) #"<could not resolve type>"])

        lldbutil.continue_to_breakpoint(process, generic_bkpt)
        # FIXME: this is formatted incorrectly.
        self.expect("fr var -d no-dynamic t", substrs=["(T)"]) #, "world"])

    @requireNotEmbeddedSwift # embedded Swift monomorphizes every generic, so the parameter is never typed (T)
    @swiftTest
    @skipIf(debug_info=no_match(["dwarf"]))
    def test_missing_module(self):
        """Test what happens when a .swiftmodule can't be loaded"""
        self.prepare()
        target, process, _, _ = lldbutil.run_to_name_breakpoint(self, 'main')
        self.run_tests(target, process)

    @requireNotEmbeddedSwift # embedded Swift monomorphizes every generic, so the parameter is never typed (T)
    @swiftTest
    @skipIf(debug_info=no_match(["dwarf"]))
    def test_damaged_module(self):
        """Test what happens when a .swiftmodule can't be loaded"""
        self.prepare()
        with open(self.getBuildArtifact("a.swiftmodule"), 'w') as mod:
            mod.write('I am damaged.\n')

        target, process, _, _ = lldbutil.run_to_name_breakpoint(self, 'main')
        self.run_tests(target, process)

    # Only the Darwin build links the .swiftmodule by reference
    # (-add_ast_path), so only there does damaging the file on disk reach the
    # deserializer.
    @skipUnlessDarwin
    @swiftTest
    @skipIf(debug_info=no_match(["dwarf"]))
    def test_damaged_module_diagnostic(self):
        """Test that the deserialization failure is reported to the user"""
        self.prepare()
        with open(self.getBuildArtifact("a.swiftmodule"), 'w') as mod:
            mod.write('I am damaged.\n')

        # The error goes to the debugger's error stream rather than to the
        # result of the command that triggers it, so capture the stream.
        log = self.getBuildArtifact("stderr.log")
        with open(log, "w") as f:
            self.assertSuccess(
                self.dbg.SetErrorFile(lldb.SBFile(f.fileno(), "w", False)))
            lldbutil.run_to_name_breakpoint(self, 'main')
            # Evaluating anything forces a SwiftASTContext to be created.
            self.expect("expression 1", substrs=["1"])

        self.filecheck_log(log, __file__)
        # CHECK: The serialized module is corrupted.
