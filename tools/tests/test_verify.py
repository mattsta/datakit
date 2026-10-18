"""``dktools verify``: log scanners, ctest arguments and the result contract.

The end-to-end cases replace the program runner with canned logs, so
they check the orchestration and exit codes without a real build.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from dktools import steps, cli, toolchain, verify
from dktools.steps import Step
from dktools.gitutil import ToolError

from repofixture import IsolatedGitEnv, TempRepo

CTEST_PASS = """\
Test project /w/build
    Start 1: databox
1/1 Test #1: databox ..........   Passed    0.10 sec

100% tests passed, 0 tests failed out of 1
"""

CTEST_FAIL = """\
50% tests passed, 2 tests failed out of 4

The following tests did not run:
\t  3 - skipme (Skipped)

The following tests FAILED:
\t  1 - flex (Failed)
\t  2 - multimapP (Timeout)
Errors while running CTest
"""


class Scanners(unittest.TestCase):
    def setUp(self) -> None:
        self._dir = tempfile.TemporaryDirectory()
        self.dir = Path(self._dir.name)

    def tearDown(self) -> None:
        self._dir.cleanup()

    def log(self, name: str, text: str) -> Path:
        path = self.dir / name
        path.write_text(text)
        return path

    def test_parse_ctest(self) -> None:
        ok = steps.parse_ctest(self.log("ok.log", CTEST_PASS))
        self.assertEqual(ok.summary, "100% tests passed, 0 tests failed out of 1")
        self.assertEqual(ok.failed, [])
        # CMake 4 drops the failure count when nothing failed.
        cmake4 = self.log("cmake4.log", "100% tests passed out of 1\n")
        self.assertEqual(
            steps.parse_ctest(cmake4).summary, "100% tests passed out of 1"
        )
        bad = steps.parse_ctest(self.log("bad.log", CTEST_FAIL))
        self.assertEqual(bad.failed, ["flex", "multimapP"])
        self.assertEqual(bad.not_run, ["skipme"])
        none = steps.parse_ctest(self.log("none.log", "No tests were found!!!\n"))
        self.assertTrue(none.no_tests)
        self.assertEqual(steps.parse_ctest(self.dir / "absent.log").summary, "")

    def test_warnings_dedup_and_grouping(self) -> None:
        tree = self.dir / "tree"
        text = (
            f"{tree}/src/flex.c:10:5: warning: unused variable 'x' [-Wunused-variable]\n"
            f"{tree}/src/flex.c:10:5: warning: unused variable 'x' [-Wunused-variable]\n"
            f"{tree}/deps/varint/src/v.c:3:1: warning: shadow [-Wshadow]\n"
            # gcc names headers by their include path.
            f"{tree}/src/../deps/varint/src/w.h:7:2: warning: sign [-Wsign-compare]\n"
            "ld: warning: object file built for newer macOS\n"
            "some unrelated line\n"
        )
        log = self.log("build.log", text)
        self.assertEqual(steps.count_warning_lines(log), 5)
        found = steps.warnings(log, tree)
        self.assertEqual(len(found), 4)
        groups = steps.group_warnings(found)
        self.assertEqual(groups["src"], {"src/flex.c": 1})
        self.assertEqual(
            groups["deps"], {"deps/varint/src/v.c": 1, "deps/varint/src/w.h": 1}
        )
        self.assertEqual(groups["other"], {"(no source location)": 1})

    def test_linker_notes_are_not_warnings(self) -> None:
        tree = self.dir / "tree"
        note = "warning: no debug symbols in executable (-arch arm64)"
        text = (
            f"{note}\n"
            "[  5%] Built target varint\n"
            f"  {note}  \n"
            "warning: no debug symbols in executable (-arch x86_64)\n"
            # Anything else shaped like a warning still counts.
            f"{tree}/src/a.c:1:1: warning: no debug symbols in executable "
            "(-arch arm64)\n"
            "ld: warning: no debug symbols in executable (-arch arm64)\n"
            f"{note} extra\n"
            "warning: no debug symbols in executable\n"
        )
        log = self.log("notes.log", text)
        self.assertEqual(steps.count_linker_notes(log), 3)
        self.assertEqual(steps.count_warning_lines(log), 4)
        self.assertEqual(len(steps.warnings(log, tree)), 4)
        self.assertTrue(steps.is_linker_note(note))
        self.assertFalse(steps.is_linker_note("ld: " + note))

    def test_sanitizer_reports(self) -> None:
        log = self.log(
            "out.log",
            "fine\nsrc/a.c:3:9: runtime error: signed integer overflow\n"
            "==1==ERROR: AddressSanitizer: heap-use-after-free\n"
            "==2==ERROR: LeakSanitizer: detected memory leaks\n",
        )
        hits = steps.sanitizer_reports([log, self.dir / "absent.log"])
        self.assertEqual([h.line for h in hits], [2, 3, 4])

    def test_ctest_command(self) -> None:
        cmd = steps.ctest_command(Path("b"), 8, "a|b", exclude=["c.*P", ""])
        self.assertIn("--no-tests=error", cmd)
        self.assertEqual(cmd[cmd.index("-R") + 1], "^(a|b)$")
        self.assertEqual(cmd[cmd.index("-E") + 1], "^(c.*P)$")
        self.assertNotIn("-E", steps.ctest_command(Path("b"), 1, "x", exclude=[""]))

    def test_parse_specs(self) -> None:
        self.assertEqual(
            verify.parse_specs(["src/a.c", "src/b.c=other.c"]),
            [("src/a.c", None), ("src/b.c", Path("other.c"))],
        )
        for bad in ([], ["/abs"], ["../x"], ["src/a.c="]):
            with self.subTest(bad=bad), self.assertRaises(ToolError):
                verify.parse_specs(bad)


class FakeRunner:
    """Stands in for steps.run_logged: canned output per step name."""

    def __init__(self, codes: dict[str, int], outputs: dict[str, str]) -> None:
        self.codes = codes
        self.outputs = outputs
        self.calls: list[tuple[str, list[str]]] = []

    def __call__(self, name, cmd, log, cwd=None, env=None) -> Step:
        self.calls.append((name, list(cmd)))
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(self.outputs.get(name, ""))
        if name.startswith("ctest"):
            binary_dir = Path(cmd[cmd.index("--test-dir") + 1])
            temp = binary_dir / "Testing" / "Temporary"
            temp.mkdir(parents=True, exist_ok=True)
            (temp / "LastTest.log").write_text(self.outputs.get(name + "-output", ""))
        return Step(name, log, self.codes.get(name, 0))


class VerifyRun(unittest.TestCase):
    def setUp(self) -> None:
        self._env = IsolatedGitEnv().__enter__()
        self.repo = TempRepo()
        self.repo.write(
            "CMakePresets.json",
            json.dumps({"configurePresets": [{"name": "dev"}, {"name": "debug"}]}),
        )
        self.repo.write("src/a.c", "int a;\n")
        self.repo.commit("base", "CMakePresets.json", "src/a.c")
        self.root = self.repo.path.parent / "scratch"
        exe = sys.executable
        self.compiler = toolchain.Compiler("clang22", exe, exe, exe, exe)

    def tearDown(self) -> None:
        self.repo.cleanup()
        self._env.__exit__()

    def run_verify(self, runner: FakeRunner, **kw) -> tuple[int, str]:
        opts = verify.VerifyOptions(
            name="t",
            specs=kw.pop("specs", ["src/a.c"]),
            tests=kw.pop("tests", "databox"),
            compiler=self.compiler,
            root=self.root,
            jobs=kw.pop("jobs", 4),
            **kw,
        )
        out = io.StringIO()
        with (
            mock.patch.object(steps, "run_logged", runner),
            mock.patch("dktools.gitutil.require_program", return_value="uv"),
            contextlib.redirect_stdout(out),
        ):
            code = verify.run(self.repo.path, opts)
        return code, out.getvalue()

    def test_pass_uses_only_the_named_paths(self) -> None:
        self.repo.write("src/a.c", "int a2;\n")
        self.repo.write("src/stray.c", "int stray;\n")
        runner = FakeRunner(
            {}, {"ctest": CTEST_PASS, "allocaudit": "100% clean pattern\n"}
        )
        code, out = self.run_verify(runner)
        self.assertEqual(code, 0, out)
        self.assertIn("verify t: PASS", out)
        src = self.root / "verify" / "t" / "src"
        self.assertEqual((src / "src" / "a.c").read_text(), "int a2;\n")
        self.assertFalse((src / "src" / "stray.c").exists())
        names = [name for name, _ in runner.calls]
        self.assertEqual(names, ["configure", "build", "ctest", "allocaudit"])
        configure = runner.calls[0][1]
        self.assertIn("--preset", configure)
        self.assertIn(f"-DCMAKE_C_COMPILER={sys.executable}", configure)
        self.assertTrue((self.root / "verify" / "t" / "result.txt").exists())

    def test_failures_exit_1(self) -> None:
        cases = {
            "warning": (
                {},
                {"build": "src/a.c:1:1: warning: x\n", "ctest": CTEST_PASS},
            ),
            "test failure": ({"ctest": 8}, {"ctest": CTEST_FAIL}),
            "sanitizer": (
                {},
                {"ctest": CTEST_PASS, "ctest-output": "a.c:1: runtime error: x\n"},
            ),
            "alloc": ({"allocaudit": 1}, {"ctest": CTEST_PASS}),
            "configure": ({"configure": 1}, {}),
        }
        for label, (codes, outputs) in cases.items():
            with self.subTest(label=label):
                code, out = self.run_verify(FakeRunner(codes, outputs))
                self.assertEqual(code, 1, out)
                self.assertIn("verify t: FAIL", out)
        code, out = self.run_verify(FakeRunner({"ctest": 8}, {"ctest": CTEST_FAIL}))
        self.assertIn("failed    flex multimapP", out)

    def test_linker_notes_alone_pass(self) -> None:
        note = "warning: no debug symbols in executable (-arch arm64)\n"
        outputs = {
            "build": note * 3,
            "ctest": CTEST_PASS,
            "allocaudit": "100% clean pattern\n",
        }
        code, out = self.run_verify(FakeRunner({}, outputs))
        self.assertEqual(code, 0, out)
        self.assertIn("build     ok, 0 warning lines, 3 linker notes", out)
        outputs["build"] = note + "src/a.c:1:1: warning: x\n"
        code, out = self.run_verify(FakeRunner({}, outputs))
        self.assertEqual(code, 1, out)
        self.assertIn("build     ok, 1 warning lines, 1 linker notes", out)

    def test_serial_tests_run_alone_and_leave_the_parallel_set(self) -> None:
        runner = FakeRunner({}, {"ctest": CTEST_PASS, "ctest-serial": CTEST_PASS})
        code, _ = self.run_verify(runner, serial=".*P")
        self.assertEqual(code, 0)
        calls = dict(runner.calls)
        self.assertEqual(calls["ctest"][calls["ctest"].index("-E") + 1], "^(.*P)$")
        serial = calls["ctest-serial"]
        self.assertEqual(serial[serial.index("-j") + 1], "1")

    def test_input_errors(self) -> None:
        for kw in (
            {"specs": ["missing.c"]},
            {"tests": None},
            {"preset": "release"},
            {"jobs": 0},
        ):
            with self.subTest(kw=kw), self.assertRaises(ToolError):
                self.run_verify(FakeRunner({}, {}), **kw)

    def test_cli_reports_input_errors_in_one_line(self) -> None:
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            code = cli.main(
                [
                    "--repo",
                    str(self.repo.path),
                    "verify",
                    "bad/name",
                    "--tests",
                    "x",
                    "--root",
                    str(self.root),
                    "src/a.c",
                ]
            )
        self.assertEqual(code, 2)
        self.assertEqual(len(err.getvalue().strip().splitlines()), 1)
        self.assertNotIn("Traceback", err.getvalue())


if __name__ == "__main__":
    unittest.main()
