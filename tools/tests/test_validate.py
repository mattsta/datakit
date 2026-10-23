"""``dktools validate``: the matrix, the test sets, the summary and exit codes.

The program runner is replaced with canned logs, so these check the
orchestration without a real build.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import json
import re
import sys
import unittest
from pathlib import Path
from unittest import mock

from dktools import leakcheck, steps, toolchain, validate
from dktools.gitutil import ToolError
from dktools.steps import Step

from repofixture import IsolatedGitEnv, TempRepo

PASS = "100% tests passed out of 3\n"
# Slow sets named by the tests, so the tiers are checked whatever the
# defaults hold (they can be empty)
SLOW = "slowEverywhere|slowToo"
DEBUG_SLOW = "slowUnderSanitizers"


class MatrixRunner:
    """Stands in for steps.run_logged, keyed on "<label> <step>".

    `outputs` and `codes` take either the full key or just the step
    ("build", "ctest", "ctest-serial", "ctest-output", ...)."""

    def __init__(self, codes=None, outputs=None, timed_out=()) -> None:
        self.codes = codes or {}
        self.outputs = outputs or {}
        self.timed_out = set(timed_out)  # step names that "time out"
        self.calls: list[tuple[str, list[str]]] = []
        self.timeouts: dict[str, float | None] = {}

    def _get(self, table: dict, key: str, default):
        step = key.split(" ", 1)[-1]
        return table.get(key, table.get(step, default))

    def __call__(self, name, cmd, log, cwd=None, env=None, timeout=None) -> Step:
        self.calls.append((name, list(cmd)))
        self.timeouts[name] = timeout
        if name in self.timed_out:
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text(f"dktools: {name}: timed out after {timeout:g}s; killed\n")
            return Step(name, log, 124, timed_out=True)
        log.parent.mkdir(parents=True, exist_ok=True)
        default = (
            PASS if name.endswith("ctest") or name.endswith("ctest-serial") else ""
        )
        log.write_text(self._get(self.outputs, name, default))
        if cmd[0] == "ctest":
            temp = Path(cmd[cmd.index("--test-dir") + 1]) / "Testing" / "Temporary"
            temp.mkdir(parents=True, exist_ok=True)
            (temp / "LastTest.log").write_text(
                self._get(self.outputs, name + "-output", "")
            )
        return Step(name, log, self._get(self.codes, name, 0))

    def command(self, name: str) -> list[str]:
        return dict(self.calls)[name]


class ValidateRun(unittest.TestCase):
    def setUp(self) -> None:
        self._env = IsolatedGitEnv().__enter__()
        self.repo = TempRepo()
        self.repo.write(
            "CMakePresets.json",
            json.dumps(
                {
                    "configurePresets": [
                        {"name": "dev"},
                        {"name": "debug"},
                        {"name": "tsan"},
                    ]
                }
            ),
        )
        self.repo.write("src/a.c", "int a;\n")
        self.base = self.repo.commit("base", "CMakePresets.json", "src/a.c")
        self.repo.write("src/b.c", "int b;\n")
        self.head = self.repo.commit("add b", "src/b.c")
        self.root = self.repo.path.parent / "scratch"
        exe = sys.executable
        self.compilers = [
            toolchain.Compiler("clang22", exe, exe, exe, exe),
            toolchain.Compiler("gcc15", exe, exe),
        ]

    def tearDown(self) -> None:
        self.repo.cleanup()
        self._env.__exit__()

    def default_dir(self) -> str:
        """The default matrix's work directory for HEAD"""
        return f"{self.head[:10]}-clang22-dev+clang22-debug+gcc15-release"

    def run_validate(self, runner: MatrixRunner, **kw) -> tuple[int, str]:
        opts = validate.ValidateOptions(
            compilers=kw.pop("compilers", self.compilers),
            leak_base=kw.pop("leak_base", self.base),
            root=self.root,
            jobs=4,
            **kw,
        )
        out = io.StringIO()
        with (
            mock.patch.object(steps, "run_logged", runner),
            mock.patch("dktools.gitutil.require_program", return_value="uv"),
            contextlib.redirect_stdout(out),
        ):
            code = validate.run(self.repo.path, opts)
        return code, out.getvalue()

    def test_pass_runs_the_whole_matrix(self) -> None:
        runner = MatrixRunner(outputs={"allocaudit": "100% clean pattern\n"})
        code, out = self.run_validate(runner, slow=SLOW, debug_slow=DEBUG_SLOW)
        self.assertEqual(code, 0, out)
        self.assertIn(": PASS", out)
        names = [name for name, _ in runner.calls]
        for label in ("clang22-dev", "clang22-debug", "gcc15-release"):
            for step in ("configure", "build", "ctest", "ctest-serial"):
                self.assertIn(f"{label} {step}", names)
        self.assertEqual(names[-1], "allocaudit")

        # All targets: no --target.
        self.assertNotIn("--target", runner.command("clang22-dev build"))
        gcc = runner.command("gcc15-release configure")
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", gcc)
        self.assertNotIn("--preset", gcc)

        # The default quick tier skips the slow set on top of --skip.
        par = runner.command("clang22-dev ctest")
        self.assertEqual(
            par[par.index("-E") + 1],
            f"^({validate.DEFAULT_SERIAL}|{validate.DEFAULT_SKIP}|{SLOW})$",
        )
        ser = runner.command("clang22-dev ctest-serial")
        self.assertEqual(ser[ser.index("-j") + 1], "1")
        self.assertEqual(ser[ser.index("-R") + 1], f"^({validate.DEFAULT_SERIAL})$")

        # Debug also skips its own slow set in the quick tier, and says so.
        dbg = runner.command("clang22-debug ctest")
        self.assertTrue(dbg[dbg.index("-E") + 1].endswith(f"|{SLOW}|{DEBUG_SLOW})$"))
        self.assertIn("- tier: quick (iterative", out)
        self.assertIn("clang22-debug: slow, skipped in the quick tier: ", out)

        summary = self.root / "validate" / self.default_dir() / "summary.md"
        self.assertEqual(summary.read_text(), out.rsplit("summary: ", 1)[0])
        # The export is the commit, not the working tree.
        self.repo.write("src/stray.c", "int stray;\n")
        self.assertTrue((summary.parent / "src" / "src" / "b.c").exists())

    def test_tsan_skips_its_own_set_and_says_so(self) -> None:
        runner = MatrixRunner(outputs={"allocaudit": "100% clean pattern\n"})
        code, out = self.run_validate(
            runner,
            compilers=self.compilers[:1],
            presets=["dev", "tsan"],
            slow=SLOW,
            debug_slow=DEBUG_SLOW,
        )
        self.assertEqual(code, 0, out)
        quick = f"|{validate.DEFAULT_TSAN_SKIP}|{SLOW}|{DEBUG_SLOW})$"
        cmd = runner.command("clang22-tsan ctest")
        self.assertTrue(cmd[cmd.index("-E") + 1].endswith(quick), cmd)
        # Every serial test is skipped under tsan, so the serial step does
        # not run (and passes) instead of failing on "no tests".
        names = [name for name, _ in runner.calls]
        self.assertNotIn("clang22-tsan ctest-serial", names)
        self.assertIn("clang22-dev ctest-serial", names)
        self.assertIn("| clang22-tsan |", out)
        self.assertNotIn("FAIL", out.split("## Skipped tests")[0])
        self.assertIn(
            "clang22-tsan: cannot be judged here: dkTestNonceLogEvictionPerf", out
        )
        # dev skips neither the tsan set nor the debug/tsan slow set.
        dev = runner.command("clang22-dev ctest")
        self.assertNotIn(DEBUG_SLOW, dev[dev.index("-E") + 1])

    def test_serial_for_drops_names_a_build_skips(self) -> None:
        self.assertEqual(validate.serial_for("a|b", ["x|b", ""]), "a")
        self.assertEqual(validate.serial_for("a", ["a"]), "")
        # A pattern is kept unless a skip names it literally
        self.assertEqual(validate.serial_for("a.*|b", ["b|c"]), "a.*")

    def test_full_tier_runs_the_slow_tests(self) -> None:
        runner = MatrixRunner(outputs={"allocaudit": "100% clean pattern\n"})
        code, out = self.run_validate(
            runner,
            compilers=self.compilers[:1],
            presets=["dev", "tsan"],
            full=True,
            slow=SLOW,
            debug_slow=DEBUG_SLOW,
        )
        self.assertEqual(code, 0, out)
        dev = runner.command("clang22-dev ctest")
        self.assertEqual(
            dev[dev.index("-E") + 1],
            f"^({validate.DEFAULT_SERIAL}|{validate.DEFAULT_SKIP})$",
        )
        tsan = runner.command("clang22-tsan ctest")
        self.assertTrue(
            tsan[tsan.index("-E") + 1].endswith(f"|{validate.DEFAULT_TSAN_SKIP})$")
        )
        self.assertIn("- tier: full (milestone", out)
        self.assertNotIn("skipped in the quick tier", out)

    def test_empty_slow_sets_skip_nothing_extra(self) -> None:
        runner = MatrixRunner(outputs={"allocaudit": "100% clean pattern\n"})
        code, out = self.run_validate(
            runner,
            compilers=self.compilers[:1],
            presets=["dev", "debug"],
            slow="",
            debug_slow="",
        )
        self.assertEqual(code, 0, out)
        for label in ("clang22-dev", "clang22-debug"):
            cmd = runner.command(f"{label} ctest")
            self.assertEqual(
                cmd[cmd.index("-E") + 1],
                f"^({validate.DEFAULT_SERIAL}|{validate.DEFAULT_SKIP})$",
            )
        self.assertIn("- tier: quick (iterative", out)
        self.assertNotIn("skipped in the quick tier", out)

    def test_tsan_reports_fail_the_run(self) -> None:
        runner = MatrixRunner(
            outputs={
                "allocaudit": "100% clean pattern\n",
                "clang22-tsan ctest-output": "1/2 Testing: fastmutex\n"
                "WARNING: ThreadSanitizer: data race (pid=1)\n",
            }
        )
        code, out = self.run_validate(
            runner, compilers=self.compilers[:1], presets=["tsan"]
        )
        self.assertEqual(code, 1, out)
        self.assertIn("fastmutex: WARNING: ThreadSanitizer: data race", out)

    def test_two_matrices_on_one_revision_keep_separate_logs(self) -> None:
        runner = MatrixRunner(outputs={"allocaudit": "100% clean pattern\n"})
        code, out = self.run_validate(runner)
        self.assertEqual(code, 0, out)
        tsan = MatrixRunner(outputs={"allocaudit": "100% clean pattern\n"})
        code, out = self.run_validate(
            tsan, compilers=self.compilers[:1], presets=["tsan"]
        )
        self.assertEqual(code, 0, out)
        base = self.root / "validate"
        self.assertTrue((base / self.default_dir() / "summary.md").exists())
        self.assertTrue(
            (base / f"{self.head[:10]}-clang22-tsan" / "summary.md").exists()
        )

    def test_warnings_are_split_by_area(self) -> None:
        tree = (self.root / "validate" / self.default_dir() / "src").resolve()
        build = (
            f"{tree}/src/a.c:1:5: warning: unused [-Wunused]\n"
            f"{tree}/deps/varint/src/v.c:2:1: warning: shadow [-Wshadow]\n"
        )
        runner = MatrixRunner(outputs={"gcc15-release build": build})
        code, out = self.run_validate(runner)
        self.assertEqual(code, 1)
        gcc = out.split("### gcc15-release", 1)[1]
        self.assertIn("- datakit src/: 1\n  - `src/a.c`: 1", gcc)
        self.assertIn("- deps/: 1\n  - `deps/varint/src/v.c`: 1", gcc)

    def test_default_serial_set_is_the_timing_sensitive_tests(self) -> None:
        serial = re.compile(steps.anchored(validate.DEFAULT_SERIAL))
        self.assertRegex("dkTestNonceLogEvictionPerf", serial)
        # The persist tests use private scratch directories, and the
        # keyring race test is deterministic: both run in parallel.
        for name in (
            "flexP",
            "multimapP",
            "persist",
            "persistCtx",
            "dkTestKeyringGetRawCopyMigrationCluster",
            "databox",
        ):
            self.assertNotRegex(name, serial)

    def test_linker_notes_are_counted_but_pass(self) -> None:
        note = "warning: no debug symbols in executable (-arch arm64)\n"
        runner = MatrixRunner(
            outputs={
                "gcc15-release build": note * 2,
                "allocaudit": "100% clean pattern\n",
            }
        )
        code, out = self.run_validate(runner)
        self.assertEqual(code, 0, out)
        self.assertIn("| 0 distinct (0 lines; 2 linker notes) |", out)
        gcc = out.split("### gcc15-release", 1)[1]
        self.assertIn("- other: 0", gcc)

    def test_failures_exit_1(self) -> None:
        cases = {
            "test failure": (
                {"ctest": 8},
                {"ctest": "The following tests FAILED:\n\t  1 - flex (Failed)\n"},
                "clang22-dev: flex",
            ),
            "sanitizer": (
                {},
                {
                    "ctest-serial-output": "1/1 Testing: flexP\n"
                    "x.c:1:1: runtime error: overflow\n"
                },
                "ctest-serial-output.log:2` flexP: x.c:1:1: runtime error: overflow",
            ),
            "build": ({"clang22-debug build": 2}, {}, "FAIL (2)"),
            "alloc": (
                {"allocaudit": 1},
                {"allocaudit": "VIOLATION x\n"},
                "VIOLATION x",
            ),
        }
        for label, (codes, outputs, needle) in cases.items():
            with self.subTest(label=label):
                code, out = self.run_validate(MatrixRunner(codes, outputs))
                self.assertEqual(code, 1, out)
                self.assertIn(needle, out)

    def test_every_step_has_a_timeout(self) -> None:
        runner = MatrixRunner(outputs={"allocaudit": "100% clean pattern\n"})
        code, out = self.run_validate(runner)
        self.assertEqual(code, 0, out)
        for name, timeout in runner.timeouts.items():
            with self.subTest(step=name):
                self.assertIsNotNone(timeout)
                self.assertGreater(timeout, 0)
        ctest = runner.command("clang22-dev ctest")
        self.assertEqual(
            ctest[ctest.index("--timeout") + 1], str(steps.CTEST_TEST_TIMEOUT)
        )

    def test_timed_out_steps_fail_and_say_so(self) -> None:
        for step in ("clang22-dev build", "clang22-dev ctest", "allocaudit"):
            with self.subTest(step=step):
                runner = MatrixRunner(timed_out=[step])
                code, out = self.run_validate(runner)
                self.assertEqual(code, 1, out)
                self.assertIn("FAIL (timed out)", out)

    def test_leakage_gate_over_the_range(self) -> None:
        # Only leakcheck.py and its tests may spell the terms out.
        term = leakcheck.HARD_TERMS[0].label
        self.repo.write("src/c.c", f"/* {term} */\n")
        self.head = self.repo.commit("leak", "src/c.c")
        code, out = self.run_validate(MatrixRunner())
        self.assertEqual(code, 1)
        self.assertIn(f"src/c.c:1: [{term}]", out)
        code, out = self.run_validate(MatrixRunner(), leak_base="")
        self.assertEqual(code, 0, out)
        self.assertIn("skipped (empty --leak-base)", out)

    def test_input_errors(self) -> None:
        for kw in (
            {"rev": "no-such-rev"},
            {"leak_base": "no-such-base"},
            {"presets": ["perf"]},
            {"compilers": []},
            {"compilers": [toolchain.Compiler("clang22", "/nonexistent/cc", "x")]},
        ):
            with self.subTest(kw=kw), self.assertRaises(ToolError):
                self.run_validate(MatrixRunner(), **kw)


if __name__ == "__main__":
    unittest.main()
