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

        with _managed_async(self.dbg):
            # Suspend the current thread.
            thread.Suspend()
            # Continue - other threads only.
            self.dbg.SetAsync(True)
            process.Continue()
            # Wait - allowing other threads to run.
            time.sleep(5)
            # Stop the threads.
            self.dbg.SetAsync(False)
            self.dbg.HandleCommand("process interrupt")
            # Note: After a single interrupt, lldb reports the process as
            # running, but two interrupt calls results in a stopped process.
            # Also, using `process.Stop()` instead of `"process interrupt"`
            # did not work.
            self.dbg.HandleCommand("process interrupt")

        # Get the frame for the the breakpoint hit in `main()`.
        frame = lldb.SBFrame()
        for t in process.threads:
            if t.stop_reason == lldb.eStopReasonBreakpoint:
                if "Entry.main()" in t.frame[0].name:
                    frame = t.frame[0]
                    break
        self.assertTrue(frame.IsValid())

        defaultActor = frame.var("a.$defaultActor")
        unprioritised_jobs = defaultActor.GetChildMemberWithName("unprioritised_jobs")
        self.assertTrue(unprioritised_jobs.IsValid())

        # There are 4 child tasks (async let), the first one occupies the actor
        # with a sleep, the next 3 go on to the queue.
        self.assertEqual(unprioritised_jobs.num_children, 3)
        self.assertEqual(defaultActor.summary, "running")
        for job in unprioritised_jobs:
            self.assertRegex(job.name, r"^\d+")
            self.assertRegex(job.summary, r"^id:[1-9]\d* flags:\S+")
