import re

import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import TestBase
import lldbsuite.test.lldbutil as lldbutil


class TestCase(TestBase):

    @requireNotEmbeddedSwift
    @skipUnlessPlatform(["macosx", "linux"])
    @swiftTest
    def test_awaiting_tasks(self):
        """Test the frames of tasks awaiting an async let child."""
        self.build()
        lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )
        self.runCmd("language swift task list")
        awaiting = re.findall(
            r"Task (\d+), addr = (0x[0-9a-fA-F]+) \[awaiting Task \d+\]",
            self.res.GetOutput(),
        )
        awaiting.sort(key=lambda task: int(task[0]))
        self.assertEqual(len(awaiting), 3)

        # Task 1 runs fact(n: 4) and awaits Task 2, which runs fact(n: 3), etc.
        for (_, addr), n in zip(awaiting, [4, 3, 2]):
            self.expect(
                f"language swift task backtrace {addr}",
                patterns=[
                    rf"frame #0: 0x[0-9a-f]+ a\.out`fact\(n={n}\) at main\.swift:11"
                ],
            )
            self.runCmd(f"language swift task select {addr}")
            self.expect("frame variable n", substrs=[f"n = {n}"])
