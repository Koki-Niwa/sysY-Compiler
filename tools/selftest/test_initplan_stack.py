"""Regression: initplan workers must not mutate process-global stack size."""

import concurrent.futures
import threading
import unittest
from unittest.mock import patch

from tools.selftest.check_initplan import run_with_big_stack


class InitplanStackTests(unittest.TestCase):
    def test_concurrent_checks_do_not_change_default_thread_stack(self):
        # The module sets the large default once, before workers are started.
        # Any worker-local stack_size call would be a shared-state race.
        with patch('threading.stack_size', side_effect=AssertionError(
                'worker changed process-global stack size')):
            with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
                actual = list(pool.map(lambda n: run_with_big_stack(lambda: n * n),
                                       range(16)))
        self.assertEqual(actual, [n * n for n in range(16)])


if __name__ == '__main__':
    unittest.main()
