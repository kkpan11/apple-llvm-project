import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import TestBase
import lldbsuite.test.lldbutil as lldbutil

import re

def _tail(output):
    """Delete the first line of output text."""
    result, _ = re.subn(r"^.*\n", "", output, count=1)
    return result


class TestCase(TestBase):

    @skipEmbeddedSwiftOnLinux
    @skipUnlessPlatform(["macosx", "linux"])
    @swiftTest
    def test_compare_printed_task_variable_to_task_info(self):
        """Compare the output of a printed Task to the output of `task info`."""
        self.build()
        lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )
        self.runCmd("frame variable task")
        frame_variable_output = self.res.GetOutput()
        self.runCmd("language swift task info")
        task_info_output = self.res.GetOutput()
        self.assertEqual(_tail(task_info_output), _tail(frame_variable_output))

    @skipEmbeddedSwiftOnLinux
    @skipUnlessPlatform(["macosx", "linux"])
    @swiftTest
    def test_compare_printed_task_variable_to_task_info_with_address(self):
        """Compare the output of a printed Task to the output of `task info <address>`."""
        self.build()
        _, _, thread, _ = lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )

        frame = thread.frames[0]
        task = frame.FindVariable("task")
        task_addr = task.GetChildMemberWithName("address").unsigned

        self.runCmd("frame variable task")
        frame_variable_output = self.res.GetOutput()
        self.runCmd(f"language swift task info {task_addr}")
        task_info_output = self.res.GetOutput()
        self.assertEqual(_tail(task_info_output), _tail(frame_variable_output))

    @skipEmbeddedSwiftOnLinux
    @skipUnlessPlatform(["macosx", "linux"])
    @swiftTest
    def test_task_info_without_current_task(self):
        """Test `task info` on a thread that is not running a task."""
        self.build()
        # The C entry point runs before the main task is created.
        lldbutil.run_to_name_breakpoint(self, "main")
        self.expect(
            "language swift task info",
            error=True,
            substrs=["no task is running on the current thread"],
        )
