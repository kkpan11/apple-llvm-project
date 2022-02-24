# TestSwiftMoveFunction.py
#
# This source file is part of the Swift.org open source project
#
# Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
# Licensed under Apache License v2.0 with Runtime Library Exception
#
# See https://swift.org/LICENSE.txt for license information
# See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
#
# ------------------------------------------------------------------------------
"""
Check that we properly show variables at various points of the CFG while
stepping with the move function.
"""
import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil
import os
import sys
import unittest2

def stderr_print(line):
    sys.stderr.write(line + "\n")

class TestSwiftMoveFunctionType(TestBase):

    mydir = TestBase.compute_mydir(__file__)

    @skipUnlessPlatform(["macosx"])
    @swiftTest
    def test_swift_move_function(self):
        """Check that we properly show variables at various points of the CFG while
        stepping with the move function.
        """
        self.build()

        self.exec_artifact = self.getBuildArtifact(self.exec_name)
        self.target = self.dbg.CreateTarget(self.exec_artifact)
        self.assertTrue(self.target, VALID_TARGET)

        self.do_setup_breakpoints()

        self.process = self.target.LaunchSimple(None, None, os.getcwd())
        threads = lldbutil.get_threads_stopped_at_breakpoint(
            self.process, self.breakpoints[0])
        self.assertEqual(len(threads), 1)
        self.thread = threads[0]

        self.do_check_copyable_value_test()
        self.do_check_copyable_var_test()
        self.do_check_addressonly_value_test()
        self.do_check_addressonly_var_test()
        # ccf is conditional control flow
        self.do_check_copyable_value_ccf_true()
        self.do_check_copyable_value_ccf_false()
        self.do_check_copyable_var_ccf_true_reinit_out_block()
        self.do_check_copyable_var_ccf_true_reinit_in_block()
        self.do_check_copyable_var_ccf_false_reinit_out_block()
        self.do_check_copyable_var_ccf_false_reinit_in_block()

    def setUp(self):
        TestBase.setUp(self)
        self.main_source = "main.swift"
        self.main_source_spec = lldb.SBFileSpec(self.main_source)
        self.exec_name = "a.out"

    def add_breakpoints(self, name, num_breakpoints):
        pattern = 'Set breakpoint {} here {}'
        for i in range(num_breakpoints):
            pat = pattern.format(name, i+1)
            brk = self.target.BreakpointCreateBySourceRegex(
                pat, self.main_source_spec)
            self.assertGreater(brk.GetNumLocations(), 0, VALID_BREAKPOINT)
            yield brk

    def get_var(self, name):
        frame = self.thread.frames[0]
        self.assertTrue(frame.IsValid(), "Couldn't get a frame.")
        return frame.FindVariable(name)

    def do_setup_breakpoints(self):
        self.breakpoints = []

        self.breakpoints.extend(
            self.add_breakpoints('copyableValueTest', 3))
        self.breakpoints.extend(
            self.add_breakpoints('addressOnlyValueTest', 3))
        self.breakpoints.extend(
            self.add_breakpoints('copyableVarTest', 4))
        self.breakpoints.extend(
            self.add_breakpoints('addressOnlyVarTest', 4))
        self.breakpoints.extend(
            self.add_breakpoints('copyableValueCCFTrueTest',
                                 5))
        self.breakpoints.extend(
            self.add_breakpoints('copyableValueCCFFalseTest',
                                 3))
        self.breakpoints.extend(
            self.add_breakpoints('copyableVarTestCCFlowTrueReinitOutOfBlockTest',
                                 5))
        self.breakpoints.extend(
            self.add_breakpoints('copyableVarTestCCFlowTrueReinitInBlockTest',
                                 5))
        self.breakpoints.extend(
            self.add_breakpoints('copyableVarTestCCFlowFalseReinitOutOfBlockTest',
                                 4))
        self.breakpoints.extend(
            self.add_breakpoints('copyableVarTestCCFlowFalseReinitInBlockTest', 3))

    def do_check_copyable_value_test(self):
        # We haven't defined varK yet.
        varK = self.get_var('k')

        self.assertIsNone(varK.value, "varK initialized too early?!")

        # Go to break point 2. k should be valid.
        self.runCmd('continue')
        self.assertGreater(varK.unsigned, 0, "varK not initialized?!")

        # Go to breakpoint 3. k should no longer be valid.
        self.runCmd('continue')
        self.assertIsNone(varK.value, "K is live but was moved?!")

        # Run so we hit the next breakpoint to jump to the next test's
        # breakpoint.
        self.runCmd('continue')

    def do_check_copyable_var_test(self):
        # We haven't defined varK yet.
        varK = self.get_var('k')
        self.assertIsNone(varK.value, "varK initialized too early?!")

        # Go to break point 2. k should be valid.
        self.runCmd('continue')
        self.assertGreater(varK.unsigned, 0, "varK not initialized?!")

        # Go to breakpoint 3. We invalidated k
        self.runCmd('continue')
        self.assertIsNone(varK.value, "K is live but was moved?!")

        # Go to the last breakpoint and make sure that k is reinitialized
        # properly.
        self.runCmd('continue')
        self.assertGreater(varK.unsigned, 0, "varK not initialized")

        # Run so we hit the next breakpoint to go to the next test.
        self.runCmd('continue')

    def do_check_addressonly_value_test(self):
        # We haven't defined varK yet.
        varK = self.get_var('k')

        # Go to break point 2. k should be valid and m should not be. Since M is
        # a dbg.declare it is hard to test robustly that it is not initialized
        # so we don't do so. We have an additional llvm.dbg.addr test where we
        # move the other variable and show the correct behavior with
        # llvm.dbg.declare.
        self.runCmd('continue')
        self.assertGreater(varK.unsigned, 0, "var not initialized?!")

        # Go to breakpoint 3.
        self.runCmd('continue')
        self.assertEqual(varK.unsigned, 0,
                        "dbg thinks varK is live despite move?!")

        # Run so we hit the next breakpoint as part of the next test.
        self.runCmd('continue')

    def do_check_addressonly_var_test(self):
        varK = self.get_var('k')

        # Go to break point 2. k should be valid.
        self.runCmd('continue')
        self.assertGreater(varK.unsigned, 0, "varK not initialized?!")

        # Go to breakpoint 3. K was invalidated.
        self.runCmd('continue')
        self.assertIsNone(varK.value, "K is live but was moved?!")

        # Go to the last breakpoint and make sure that k is reinitialized
        # properly.
        self.runCmd('continue')
        self.assertGreater(varK.unsigned, 0, "varK not initialized")

        # Run so we hit the next breakpoint as part of the next test.
        self.runCmd('continue')

    def do_check_copyable_value_ccf_true(self):
        varK = self.get_var('k')

        # Check at our start point that we do not have any state for varK and
        # then continue to our next breakpoint.
        self.assertIsNone(varK.value, "varK should not have a value?!")
        self.runCmd('continue')

        # At this breakpoint, k should be defined since we are going to do
        # something with it.
        self.assertIsNotNone(varK.value, "varK should have a value?!")
        self.runCmd('continue')

        # At this breakpoint, we are now in the conditional control flow part of
        # the loop. Make sure that we can see k still.
        self.assertIsNotNone(varK.value, "varK should have a value?!")
        self.runCmd('continue')

        # Ok, we just performed the move. k should not be no longer initialized.
        self.assertIsNone(varK.value, "varK should not have a value?!")
        self.runCmd('continue')

        # Finally we left the conditional control flow part of the function. k
        # should still be None.
        self.assertIsNone(varK.value, "varK should not have a value!")

        # Run again so we go and run to the next test.
        self.runCmd('continue')

    def do_check_copyable_value_ccf_false(self):
        varK = self.get_var('k')

        # Check at our start point that we do not have any state for varK and
        # then continue to our next breakpoint.
        self.assertIsNone(varK.value, "varK should not have a value?!")
        self.runCmd('continue')

        # At this breakpoint, k should be defined since we are going to do
        # something with it.
        self.assertIsNotNone(varK.value, "varK should have a value?!")
        self.runCmd('continue')

        # At this breakpoint, we are now past the end of the conditional
        # statement. We know due to the move checking that k can not have any
        # uses that are reachable from the move. So it is safe to always not
        # provide the value here.
        self.assertIsNone(varK.value, "varK should have a value?!")

        # Run again so we go and run to the next test.
        self.runCmd('continue')

    def do_check_copyable_var_ccf_true_reinit_out_block(self):
        varK = self.get_var('k')

        # At first we should not have a value for k.
        self.assertEqual(varK.unsigned, 0, "varK should be nullptr!")
        self.runCmd('continue')

        # Now we are in the conditional true block. K should be defined since we
        # are on the move itself.
        self.assertGreater(varK.unsigned, 0, "varK should not be nullptr!")
        self.runCmd('continue')

        # Now we have executed the move and we are about to run code using
        # m. Make sure that K is not available!
        self.assertEqual(varK.unsigned, 0,
                         "varK was already moved! Should be nullptr")
        self.runCmd('continue')

        # We are now out of the conditional lexical block on the line of code
        # that redefines k. k should still be not available.
        self.assertEqual(varK.unsigned, 0,
                         "varK was already moved! Should be nullptr")
        self.runCmd('continue')

        # Ok, we have now reinit k and are about to call a method on it. We
        # should be valid now.
        self.assertGreater(varK.unsigned, 0,
                           "varK should have be reinitialized?!")

        # Run again so we go and run to the next test.
        self.runCmd('continue')

    def do_check_copyable_var_ccf_true_reinit_in_block(self):
        varK = self.get_var('k')

        # At first we should not have a value for k.
        self.assertEqual(varK.unsigned, 0, "varK should be nullptr!")
        self.runCmd('continue')

        # Now we are in the conditional true block. K should be defined since we
        # are on the move itself.
        self.assertGreater(varK.unsigned, 0, "varK should not be nullptr!")
        self.runCmd('continue')

        # Now we have executed the move and we are about to reinit k but have
        # not yet. Make sure we are not available!
        self.assertEqual(varK.unsigned, 0,
                         "varK was already moved! Should be nullptr")
        self.runCmd('continue')

        # We are now still inside the conditional part of the code, but have
        # reinitialized varK.
        self.assertGreater(varK.unsigned, 0,
                           "varK was reinit! Should be valid value!")
        self.runCmd('continue')

        # We now have left the conditional part of the function. k should still
        # be available.
        self.assertGreater(varK.unsigned, 0,
                           "varK should have be reinitialized?!")

        # Run again so we go and run to the next test.
        self.runCmd('continue')

    def do_check_copyable_var_ccf_false_reinit_out_block(self):
        varK = self.get_var('k')

        # At first we should not have a value for k.
        self.assertEqual(varK.unsigned, 0, "varK should be nullptr!")
        self.runCmd('continue')

        # Now we are right above the beginning of the false check. varK should
        # still be valid.
        self.assertGreater(varK.unsigned, 0, "varK should not be nullptr!")
        self.runCmd('continue')

        # Now we are after the conditional part of the code on the reinit
        # line. Since this is reachable from the move and we haven't reinit yet,
        # k should not be available.
        self.assertEqual(varK.unsigned, 0,
                         "varK was already moved! Should be nullptr")
        self.runCmd('continue')

        # Ok, we have now reinit k and are about to call a method on it. We
        # should be valid now.
        self.assertGreater(varK.unsigned, 0,
                           "varK should have be reinitialized?!")

        # Run again so we go and run to the next test.
        self.runCmd('continue')

    def do_check_copyable_var_ccf_false_reinit_in_block(self):
        varK = self.get_var('k')

        # At first we should not have a value for k.
        self.assertEqual(varK.unsigned, 0, "varK should be nullptr!")
        self.runCmd('continue')

        # Now we are on the doSomething above the false check. So varK should be
        # valid.
        self.assertGreater(varK.unsigned, 0, "varK should not be nullptr!")
        self.runCmd('continue')

        # Now we are after the conditional scope. Since k was reinitialized in
        # the conditional scope, along all paths we are valid so varK should
        # still be available.
        self.assertGreater(varK.unsigned, 0,
                           "varK should not be nullptr?!")

        # Run again so we go and run to the next test.
        self.runCmd('continue')
