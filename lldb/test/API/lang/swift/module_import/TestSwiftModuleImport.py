import re
import sys
import lldb
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbtest as lldbtest
import lldbsuite.test.lldbutil as lldbutil
import unittest2

class TestSwiftModuleImport(lldbtest.TestBase):

    mydir = lldbtest.TestBase.compute_mydir(__file__)
    NO_DEBUG_INFO_TESTCASE = True

    @swiftTest
    def test(self):
        self.build()
        target, process, thread, bkpt = lldbutil.run_to_source_breakpoint(
            self, 'break here', lldb.SBFileSpec('main.swift'))

        log = self.getBuildArtifact("types.log")
        self.runCmd('log enable lldb types -f "%s"' % log)
        self.runCmd("expression -- 0", check=False)
        with open(log) as f:
            pat = re.compile("-linux|target|triple|x86_64-")
            for line in f.readlines():
                if pat.match(line):
                    print(line)
                    print(line, file=sys.stderr)
        self.assertTrue(False)
        self.filecheck('platform shell cat "%s"' % log, __file__)
#       CHECK: SwiftASTContextForExpressions{{.*}}Module import remark: loaded module 'a'
