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
        _, _, thread, _ = lldbutil.run_to_name_breakpoint(self, "breakHere")

        # Use the caller frame.
        frame = thread.frames[1]

        self.assertEqual(frame.var("a.data").value, "15")

        defaultActor = frame.var("a.$defaultActor")
        self.assertEqual(defaultActor.summary, "running")

        unprioritised_jobs = defaultActor.GetChildMemberWithName("unprioritised_jobs")
        # There are 4 child tasks (async let), the first one occupies the actor
        # with a call to readLine, the next 3 go on the queue.
        self.assertEqual(unprioritised_jobs.num_children, 3)
        for job in unprioritised_jobs:
            self.assertRegex(job.name, r"^\d+")
            self.assertRegex(job.summary, r"^id:[1-9]\d* flags:\S+")
