"""``dktools.procs``: timeouts, process-group kills and pool failures.

Every case uses tiny Python children from ``prochelpers`` and asserts
both the error and that no process is left behind.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import os
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

import prochelpers
from dktools import procs
from dktools.gitutil import ToolError
from prochelpers import SLEEP_FOREVER, command, wait_gone

HERE = Path(__file__).resolve().parent

#: Long enough for a loaded machine to start Python twice (the child
#: and its grandchild) before the timeout fires.
TIMEOUT = 3


class Scratch(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def wait_for_file(self, path: Path, seconds: float = 10.0) -> str:
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if path.exists() and path.read_text():
                return path.read_text()
            time.sleep(0.02)
        self.fail(f"{path.name} never written")


class Run(Scratch):
    def test_captures_both_pipes_without_deadlock(self) -> None:
        size = 1 << 20  # far beyond a pipe buffer, on both pipes
        proc = procs.run(command("noisy", str(size)), cwd=HERE, timeout=60)
        self.assertEqual(proc.returncode, 0)
        self.assertEqual(len(proc.stdout), size)
        self.assertEqual(len(proc.stderr), size)

    def test_early_exit_propagates_the_code(self) -> None:
        cmd = [sys.executable, "-c", "raise SystemExit(3)"]
        start = time.monotonic()
        self.assertEqual(procs.run(cmd, timeout=60).returncode, 3)
        self.assertLess(time.monotonic() - start, 30)

    def test_missing_program_is_a_tool_error(self) -> None:
        with self.assertRaisesRegex(ToolError, "not found on PATH"):
            procs.run(["dktools-no-such-program"])
        with self.assertRaisesRegex(ToolError, "no such directory"):
            procs.run(SLEEP_FOREVER, cwd=self.tmp / "missing")

    def test_stdin_is_not_the_terminal(self) -> None:
        cmd = [sys.executable, "-c", "import sys; print(sys.stdin.read())"]
        self.assertEqual(procs.run(cmd, timeout=60).stdout.strip(), b"")

    def test_timeout_kills_the_whole_group(self) -> None:
        pidfile = self.tmp / "grandchild.pid"
        start = time.monotonic()
        with self.assertRaises(procs.TimedOut):
            procs.run(
                command("spawn-grandchild", str(pidfile)), cwd=HERE, timeout=TIMEOUT
            )
        self.assertLess(time.monotonic() - start, TIMEOUT + procs.KILL_GRACE + 10)
        grandchild = int(self.wait_for_file(pidfile))
        self.assertEqual(wait_gone([grandchild]), [])

    def test_a_grandchild_holding_the_pipe_is_bounded_by_the_timeout(self) -> None:
        pidfile = self.tmp / "orphan.pid"
        with self.assertRaises(procs.TimedOut):
            procs.run(
                command("orphan-holding-stdout", str(pidfile)),
                cwd=HERE,
                timeout=TIMEOUT,
            )
        self.assertEqual(wait_gone([int(self.wait_for_file(pidfile))]), [])

    def test_run_to_file_timeout_keeps_output_and_kills_the_group(self) -> None:
        pidfile = self.tmp / "grandchild.pid"
        log = self.tmp / "log"
        with log.open("wb") as out:
            out.write(b"header\n")
            with self.assertRaises(procs.TimedOut):
                procs.run_to_file(
                    command("spawn-grandchild", str(pidfile)),
                    out,
                    cwd=HERE,
                    timeout=TIMEOUT,
                )
        self.assertTrue(log.read_bytes().startswith(b"header\n"))
        self.assertEqual(wait_gone([int(self.wait_for_file(pidfile))]), [])

    def test_ctrl_c_kills_the_group_before_propagating(self) -> None:
        pidfile = self.tmp / "grandchild.pid"

        def interrupt_once_started() -> None:
            # As a terminal Ctrl-C reaches only dktools: the child runs
            # in its own session.
            deadline = time.monotonic() + 30
            while not (pidfile.exists() and pidfile.read_text()):
                if time.monotonic() > deadline:
                    return  # the wait below fails the test
                time.sleep(0.02)
            os.kill(os.getpid(), signal.SIGINT)

        thread = threading.Thread(target=interrupt_once_started)
        thread.start()
        try:
            with (
                (self.tmp / "log").open("wb") as out,
                self.assertRaises(KeyboardInterrupt),
            ):
                procs.run_to_file(
                    command("spawn-grandchild", str(pidfile)),
                    out,
                    cwd=HERE,
                    timeout=60,
                )
        finally:
            thread.join()
        self.assertEqual(wait_gone([int(self.wait_for_file(pidfile))]), [])

    def test_streaming_kills_on_an_exception(self) -> None:
        with (
            tempfile.TemporaryFile() as err,
            self.assertRaises(RuntimeError),
            procs.streaming(SLEEP_FOREVER, err) as proc,
        ):
            raise RuntimeError("consumer failed")
        self.assertIsNotNone(proc.returncode)
        self.assertEqual(wait_gone([proc.pid]), [])

    def test_signals_deferred_redelivers(self) -> None:
        with self.assertRaises(KeyboardInterrupt), procs.signals_deferred():
            os.kill(os.getpid(), signal.SIGINT)
            time.sleep(0.2)  # a handler would have run by now
            finished = True
        self.assertTrue(finished)


class ParallelMap(Scratch):
    def test_results_in_item_order(self) -> None:
        items = list(range(7))
        out = procs.parallel_map(prochelpers.square, items, jobs=2, stall_timeout=60)
        self.assertEqual(out, [x * x for x in items])
        self.assertEqual(
            procs.parallel_map(prochelpers.square, [], jobs=2, stall_timeout=60), []
        )

    def test_worker_exception_names_the_item(self) -> None:
        with self.assertRaisesRegex(
            ToolError, "^item 3: ValueError: three is right out$"
        ):
            procs.parallel_map(
                prochelpers.boom_on_three,
                [1, 2, 3, 4],
                jobs=2,
                stall_timeout=60,
                describe=lambda x: f"item {x}",
            )

    def test_killed_worker_is_a_clean_error_not_a_hang(self) -> None:
        start = time.monotonic()
        with self.assertRaisesRegex(ToolError, "worker process died"):
            procs.parallel_map(
                prochelpers.die_on_three, list(range(8)), jobs=2, stall_timeout=60
            )
        self.assertLess(time.monotonic() - start, 30)

    def test_stall_timeout_kills_every_worker(self) -> None:
        start = time.monotonic()
        items = [str(self.tmp)] * 3
        with self.assertRaisesRegex(
            ToolError, r"no item finished in 1s, 3 left \(in flight: "
        ):
            procs.parallel_map(
                prochelpers.hang_recording_pid, items, jobs=2, stall_timeout=1
            )
        self.assertLess(time.monotonic() - start, 1 + procs.POOL_JOIN_GRACE + 10)
        pids = prochelpers.read_pids(self.tmp)
        self.assertTrue(pids)
        self.assertEqual(wait_gone(pids), [])

    def test_workers_exit_when_the_parent_is_killed(self) -> None:
        # The killed parent's resource tracker warns about the queue
        # semaphores it reclaims; keep that out of the test output.
        stderr = self.enterContext((self.tmp / "pool-parent.err").open("wb"))
        parent = subprocess.Popen(
            command("pool-parent", str(self.tmp)),
            cwd=HERE,
            stderr=stderr,
            start_new_session=True,
        )
        try:
            deadline = time.monotonic() + 30
            while len(prochelpers.read_pids(self.tmp)) < 2:
                self.assertLess(time.monotonic(), deadline, "workers never started")
                time.sleep(0.05)
            parent.kill()
            parent.wait()
            self.assertEqual(wait_gone(prochelpers.read_pids(self.tmp)), [])
        finally:
            if parent.poll() is None:
                os.killpg(parent.pid, signal.SIGKILL)
                parent.wait()


class Sizing(unittest.TestCase):
    def test_defaults_leave_most_of_the_machine(self) -> None:
        # 8 performance cores: 4 compile jobs, 2 tests at once, no new
        # work while the load average is 8 or more
        self.assertEqual(procs.sizing(env={}, cores=8), procs.Sizing(4, 2, 8))
        self.assertEqual(procs.sizing(env={}, cores=1), procs.Sizing(1, 1, 1))
        self.assertEqual(procs.sizing(env={}, cores=3), procs.Sizing(1, 1, 3))

    def test_jobs_and_environment_override(self) -> None:
        self.assertEqual(procs.sizing(6, env={}, cores=8), procs.Sizing(6, 3, 8))
        env = {"DKTOOLS_JOBS": "3", "DKTOOLS_TEST_JOBS": "1"}
        self.assertEqual(procs.sizing(env=env, cores=8), procs.Sizing(3, 1, 8))
        # -j wins over DKTOOLS_JOBS
        self.assertEqual(procs.sizing(2, env=env, cores=8).build, 2)

    def test_bad_sizes_refuse(self) -> None:
        for env in (
            {"DKTOOLS_JOBS": "x"},
            {"DKTOOLS_JOBS": "0"},
            {"DKTOOLS_TEST_JOBS": "-1"},
        ):
            with self.subTest(env=env), self.assertRaises(ToolError):
                procs.sizing(env=env, cores=8)
        with self.assertRaises(ToolError):
            procs.sizing(0, env={}, cores=8)

    def test_the_machine_reports_performance_cores(self) -> None:
        self.assertGreaterEqual(procs.performance_cores(), 1)


if __name__ == "__main__":
    unittest.main()
