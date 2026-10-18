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
import sys
import unittest
from pathlib import Path
from unittest import mock

from dktools import leakcheck, steps, toolchain, validate
from dktools.gitutil import ToolError
from dktools.steps import Step

from repofixture import IsolatedGitEnv, TempRepo

PASS = "100% tests passed out of 3\n"


class MatrixRunner:
    """Stands in for steps.run_logged, keyed on "<label> <step>".

    `outputs` and `codes` take either the full key or just the step
    ("build", "ctest", "ctest-serial", "ctest-output", ...)."""

    def __init__(self, codes=None, outputs=None) -> None:
        self.codes = codes or {}
        self.outputs = outputs or {}
        self.calls: list[tuple[str, list[str]]] = []

    def _get(self, table: dict, key: str, default):
        step = key.split(" ", 1)[-1]
        return table.get(key, table.get(step, default))

    def __call__(self, name, cmd, log, cwd=None, env=None) -> Step:
        self.calls.append((name, list(cmd)))
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
            json.dumps({"configurePresets": [{"name": "dev"}, {"name": "debug"}]}),
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
        code, out = self.run_validate(runner)
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

        par = runner.command("clang22-dev ctest")
        self.assertEqual(
            par[par.index("-E") + 1],
            f"^({validate.DEFAULT_SERIAL}|{validate.DEFAULT_SKIP})$",
        )
        ser = runner.command("clang22-dev ctest-serial")
        self.assertEqual(ser[ser.index("-j") + 1], "1")
        self.assertEqual(ser[ser.index("-R") + 1], f"^({validate.DEFAULT_SERIAL})$")

        # Debug also skips the slow tests, and says so.
        dbg = runner.command("clang22-debug ctest")
        self.assertTrue(dbg[dbg.index("-E") + 1].endswith("|multimap|multiFenwick)$"))
        self.assertIn("clang22-debug: also multimap, multiFenwick", out)

        summary = self.root / "validate" / self.head[:10] / "summary.md"
        self.assertEqual(summary.read_text(), out.rsplit("summary: ", 1)[0])
        # The export is the commit, not the working tree.
        self.repo.write("src/stray.c", "int stray;\n")
        self.assertTrue((summary.parent / "src" / "src" / "b.c").exists())

    def test_warnings_are_split_by_area(self) -> None:
        tree = (self.root / "validate" / self.head[:10] / "src").resolve()
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
                {"ctest-serial-output": "x.c:1:1: runtime error: overflow\n"},
                "runtime error: overflow",
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
