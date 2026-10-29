"""``dktools fuzz``: libFuzzer's statistics read, findings found, the
built targets listed, the libFuzzer command line, a target's result line.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import os
import tempfile
import time
import unittest
from pathlib import Path

from dktools import fuzz


class Statistics(unittest.TestCase):
    def test_the_last_executed_count_is_the_runs(self) -> None:
        log = (
            "#1024\tpulse  cov: 10\n"
            "stat::number_of_executed_units: 17\n"
            "stat::average_exec_per_sec:     3\n"
            "stat::number_of_executed_units: 954516\n"
        )
        self.assertEqual(fuzz.executed_runs(log), 954516)

    def test_no_statistics_is_none(self) -> None:
        self.assertIsNone(fuzz.executed_runs("==1== ERROR: libFuzzer: deadly signal\n"))


class Findings(unittest.TestCase):
    def test_finding_files_newest_last_and_nothing_else(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            now = time.time()
            for i, name in enumerate(
                ["timeout-b", "crash-a", "notes.txt", "oom-c", "leak-d"]
            ):
                p = d / name
                p.write_bytes(b"x")
                os.utime(p, (now + i, now + i))
            self.assertEqual(
                [p.name for p in fuzz.findings(d)],
                ["timeout-b", "crash-a", "oom-c", "leak-d"],
            )

    def test_a_missing_directory_has_none(self) -> None:
        self.assertEqual(fuzz.findings(Path("/nonexistent/fuzz/artifacts")), [])


class Targets(unittest.TestCase):
    def test_executable_fuzz_programs_only(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "src"
            src.mkdir()
            for name, mode in [
                ("fuzzB", 0o755),
                ("fuzzA", 0o755),
                ("fuzzNotRun", 0o644),
                ("fuzzA.o", 0o755),
                ("datakit-test", 0o755),
            ]:
                (src / name).write_bytes(b"")
                (src / name).chmod(mode)
            (src / "fuzzDir").mkdir()
            self.assertEqual(fuzz.built_targets(Path(tmp)), ["fuzzA", "fuzzB"])


class Command(unittest.TestCase):
    def test_corpus_first_seeds_when_present(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            seeds = d / "seeds"
            seeds.mkdir()
            cmd = fuzz.fuzz_command(d / "fuzzX", d / "corpus", seeds, d / "art", 30)
            self.assertEqual(cmd[0], str(d / "fuzzX"))
            self.assertIn("-max_total_time=30", cmd)
            self.assertIn(f"-timeout={fuzz.INPUT_TIMEOUT_SECONDS}", cmd)
            self.assertIn(f"-artifact_prefix={d / 'art'}/", cmd)
            self.assertEqual(cmd[-2:], [str(d / "corpus"), str(seeds)])

    def test_no_seed_directory_is_left_out(self) -> None:
        d = Path("/w")
        cmd = fuzz.fuzz_command(d / "f", d / "corpus", d / "none", d / "a", 5)
        self.assertEqual(cmd[-1], str(d / "corpus"))


class ResultLine(unittest.TestCase):
    def test_clean_found_and_failed(self) -> None:
        program = Path("/b/fuzzX")
        clean = fuzz.TargetResult("fuzzX", True, 120, 7, None, Path("/l"))
        self.assertIn("ok (120 runs, corpus 7)", clean.line(program))
        found = fuzz.TargetResult("fuzzX", False, 3, 7, Path("/a/crash-1"), Path("/l"))
        self.assertIn("FOUND crash-1", found.line(program))
        self.assertIn("reproduce: /b/fuzzX /a/crash-1", found.line(program))
        failed = fuzz.TargetResult("fuzzX", False, None, 0, None, Path("/l"))
        self.assertIn("FAILED with no input written; see /l", failed.line(program))


if __name__ == "__main__":
    unittest.main()
