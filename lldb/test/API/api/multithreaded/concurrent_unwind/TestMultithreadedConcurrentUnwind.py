"""Test that multiple threads can unwind the same thread concurrently without deadlocking."""

from lldbsuite.test.decorators import *
from lldbsuite.test.lldbtest import *


@unittest.skip("skipping due to frequent timeouts: rdar://28183131")
@requireThreadSupport
@skipIfNoSBHeaders
@skipIfRemote
@skipIfHostIncompatibleWithTarget
class MultithreadedConcurrentUnwindTestCase(TestBase):
    SHARED_BUILD_TESTCASE = False
    NO_DEBUG_INFO_TESTCASE = True

    def test(self):
        self.buildAndRunDriver(
            "driver.cpp test_concurrent_unwind.cpp", "deep_stack.cpp"
        )
