# -*- coding: utf-8 -*-
import lldb
from lldbsuite.test.decorators import *
import lldbsuite.test.lldbtest as lldbtest
import lldbsuite.test.lldbutil as lldbutil
import unittest2
import re

class TestSwiftRuntimeLibraryPath(lldbtest.TestBase):

    mydir = lldbtest.TestBase.compute_mydir(__file__)
    NO_DEBUG_INFO_TEST = True

    @swiftTest
    @skipUnlessPlatform(["macosx"])
    def test_load_link_library(self):
        """That the default runtime library path can be recovered even if
        paths weren't serialized."""
        
        # a0   a1 ⋯ aN
        # | \ /| 
        # |  X | 
        # | / \| 
        # b0   b1 ⋯ bN
        # | \ /| 
        # |  X | 
        # | / \| 
        # c0   c1 ⋯ cN
        # ⋮

        breadth = 4+1
        with open(self.getBuildArtifact("main.swift"), "w") as main:
            for i in range(1, breadth):
                main.write("import a%d\n"%i)
            for i in range(1, breadth):
                main.write("print(d%d)\n"%i)
        for i in range(1, breadth):
            with open(self.getBuildArtifact("a%d.swift"%i), "w") as a:
                for j in range(1, breadth):
                    a.write("@_exported import b%d\n"%(j))
                    a.write("public let a%d = 0\n"%j)
            with open(self.getBuildArtifact("b%d.swift"%i), "w") as b:
                for j in range(1, breadth):
                    b.write("@_exported import c%d\n"%(j))
                    b.write("public let b%d = 0\n"%j)
            with open(self.getBuildArtifact("c%d.swift"%i), "w") as c:
                for j in range(1, breadth):
                    c.write("@_exported import d%d\n"%j)
                    c.write("public let c%d = 0\n"%j)
            with open(self.getBuildArtifact("d%d.swift"%i), "w") as d:
                d.write("public let d%d = 0\n"%i)
           
        self.build()
        # Remove the .swiftmodule that carries the correct SDK path.
        log = self.getBuildArtifact("types.log")
        self.expect("log enable lldb types expr -f "+log)

        target, process, thread, bkpt = lldbutil.run_to_name_breakpoint(
            self, 'main')

        self.expect("p 1")
        logfile = open(log, "r")
        count = dict()
        for level in ["a", "b", "c", "d"]:
            for i in range(1, breadth):
                count["%s%d"%(level, i)] = 0

        regexp = re.compile('Loading link library "([a-d][0-9]+)"')
        for line in logfile:
            m = regexp.search(line)
            if m:
                count[m.group(1)] += 1

        for level in ["a", "b", "c", "d"]:
            for i in range(1, breadth):
                # LoadModule is not the only entry point, so libraries
                # found via collectLinkLibraries are still counted
                # multiple times. Uniquing them is more difficult
                # because their names may be contextual.
                self.assertLessEqual(count["%s%d"%(level, i)], 4)
