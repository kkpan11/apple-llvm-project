import time
from contextlib import contextmanager
import lldb
from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *
from lldbsuite.test import lldbutil


@contextmanager
def _managed_async(dbg):
    async_state = dbg.GetAsync()
    try:
        yield
    finally:
        dbg.SetAsync(async_state)


class TestCase(TestBase):

    @swiftTest
    def test_actor_unprioritised_jobs(self):
        """Verify that an actor exposes its unprioritised jobs (queue)."""
        self.build()
        _, process, thread, _ = lldbutil.run_to_source_breakpoint(
            self, "break here", lldb.SBFileSpec("main.swift")
        )
        frame = thread.GetSelectedFrame()
        defaultActor = frame.var("a.$defaultActor")
        unprioritised_jobs = defaultActor.GetChildMemberWithName("unprioritised_jobs")
        # There are 4 child tasks (async let), the first one occupies the actor
        # with a sleep, the next 3 go on to the queue.
        if unprioritised_jobs.num_children != 3:
            with _managed_async(self.dbg):
                # Suspend the current thread.
                thread.Suspend()
                # Continue - other threads only.
                self.dbg.SetAsync(True)
                process.Continue()
                # Wait - allow the other threads to work.
                time.sleep(2)
                # Stop the threads.
                # Notes: After a single interrupt, lldb reports the process as
                # running, but two interrupt calls results in a stopped process.
                # Also, using `process.Stop()` instead of `"process interrupt"`
                # did not work.
                self.dbg.SetAsync(False)
                self.dbg.HandleCommand("process interrupt")
                self.dbg.HandleCommand("process interrupt")
            self.expect("bt all", substrs=["abcdefghijklmnopqrstuvwxyz"])
        self.assertEqual(unprioritised_jobs.num_children, 3)
        self.assertEqual(defaultActor.summary, "running")
        for job in unprioritised_jobs:
            self.assertRegex(job.name, r"^\d+")
            self.assertRegex(job.summary, r"^id:\d+ flags:\S+")
