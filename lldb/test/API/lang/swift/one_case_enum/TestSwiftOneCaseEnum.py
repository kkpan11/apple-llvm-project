# TestSwiftOneCaseEnum.py
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
Test that an enum with only one case does not crash LLDB
"""
import lldb
from lldbsuite.test.lldbtest import *
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbutil as lldbutil


class TestSwiftOneCaseEnum(TestBase):
    @requireNotEmbeddedSwift
    @swiftTest
    def test_swift_one_case_enum(self):
        """Test that an enum with only one case does not crash LLDB"""
        self.build()
        self.do_test()

    def setUp(self):
        TestBase.setUp(self)
        self.main_source = "main.swift"
        self.main_source_spec = lldb.SBFileSpec(self.main_source)

    def do_test(self):
        """Test that an enum with only one case does not crash LLDB"""
        lldbutil.run_to_source_breakpoint(
            self, 'Set breakpoint here', self.main_source_spec)

        maybeEvent = self.frame().FindVariable("maybeEvent")
        event = self.frame().FindVariable("event")
        lldbutil.check_variable(self, maybeEvent, use_dynamic=False, use_synthetic=True, value="Goofus")
        lldbutil.check_variable(self, event, use_dynamic=False, use_synthetic=True, value="Goofus")

