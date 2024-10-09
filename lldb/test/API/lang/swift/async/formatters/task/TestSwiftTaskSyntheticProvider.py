import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


class TestCase(TestBase):
    def test(self):
        """Test Task synthetic child provider."""
        self.build()
        lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )
        # Note: The value of isEnqueued is timing dependent. For that reason,
        # the test checks only that it has a value, not what the value is.
        self.expect(
            "frame var task",
            substrs=[
                "(Task<(), Error>) task = {",
                "isChildTask = false",
                "isFuture = true",
                "isGroupChildTask = false",
                "isAsyncLetTask = false",
                "isCancelled = false",
                "isEnqueued = ",
            ],
        )
