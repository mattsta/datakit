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
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from dktools import cli, steps, toolchain, verify
from dktools.gitutil import ToolError
from dktools.steps import Step
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

#: A ctest LastTest.log: one section per test; {long} is a report.
LAST_TEST_LOG = """\
Start testing: Oct 18 12:38 EDT
----------------------------------------------------------
47/181 Testing: dod
47/181 Test: dod
Command: "/b/src/datakit-test" "test" "dod"
Directory: /b/src
"dod" start time: Oct 18 12:38 EDT
Output:
----------------------------------------------------------
{long}
<end of output>
Test time =   2.57 sec
----------------------------------------------------------
Test Passed.
"dod" end time: Oct 18 12:38 EDT
"dod" time elapsed: 00:00:02
----------------------------------------------------------

18/181 Testing: varint-dd
18/181 Test: varint-dd
Output:
varint.h:1:2: runtime error: load of misaligned address
==7==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x1
<end of output>
"""

#: ctest output for a test whose executable was never built.
CTEST_NO_EXE = """\
Test project {bindir}
      Start  1: varint-packed
Could not find executable {exe}
Looked in the following places:
{exe}
Unable to find executable: {exe}
 1/3 Test  #1: varint-packed ....................***Not Run   0.00 sec
      Start  2: skipme
 2/3 Test  #2: skipme ...........................***Not Run (Disabled)   0.00 sec
 3/3 Test  #3: databox ..........................   Passed    0.10 sec

33% tests passed, 1 tests failed out of 3

The following tests did not run:
\t  2 - skipme (Disabled)

The following tests FAILED:
\t  1 - varint-packed (Not Run)
Errors while running CTest
"""


def write_codemodel(bindir: Path, targets: dict[str, list[str]]) -> None:
    """A File API reply naming `targets` (name -> artifact paths)."""
    reply = bindir / ".cmake" / "api" / "v1" / "reply"
    reply.mkdir(parents=True, exist_ok=True)
    refs = []
    for name, paths in targets.items():
        file = f"target-{name}.json"
        artifacts = [{"path": p} for p in paths]
        (reply / file).write_text(json.dumps({"name": name, "artifacts": artifacts}))
        refs.append({"name": name, "jsonFile": file})
    model = {"configurations": [{"name": "Release", "targets": refs}]}
    (reply / "codemodel-v2-1.json").write_text(json.dumps(model))
    index = {"reply": {"codemodel-v2": {"jsonFile": "codemodel-v2-1.json"}}}
    (reply / "index-1.json").write_text(json.dumps(index))


def commands_text(commands: dict[str, list[str]]) -> str:
    """ctestCommands.cmake output for `commands`."""
    return "".join("\x1f".join([n, *c]) + "\x1e" for n, c in commands.items())


def show_only_json(commands: dict[str, list[str]]) -> str:
    """``ctest --show-only=json-v1`` output for `commands`."""
    tests = [{"name": n, "command": c} for n, c in commands.items()]
    return json.dumps({"kind": "ctestInfo", "tests": tests})


def build_targets(build: list[str]) -> list[str]:
    """The --target names of a cmake --build argv (up to the native
    tool's own arguments after --)"""
    rest = build[build.index("--target") + 1 :]
    return rest[: rest.index("--")] if "--" in rest else rest


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

    def test_lto_object_note_is_a_linker_note(self) -> None:
        lto = "warning: (arm64) /tmp/lto.o unable to open object file: No such file or directory"
        log = self.log(
            "lto.log",
            f"Generating OS X Debug Info\n{lto}\n"
            "warning: no debug symbols in executable (-arch arm64)\n"
            # Another missing object is a real problem.
            "warning: (arm64) /tmp/other.o unable to open object file: "
            "No such file or directory\n",
        )
        self.assertEqual(steps.count_linker_notes(log), 2)
        self.assertEqual(steps.count_warning_lines(log), 1)

    def test_sanitizer_reports(self) -> None:
        log = self.log(
            "out.log",
            "fine\nsrc/a.c:3:9: runtime error: signed integer overflow\n"
            "==1==ERROR: AddressSanitizer: heap-use-after-free\n"
            "==2==ERROR: LeakSanitizer: detected memory leaks\n",
        )
        hits = steps.sanitizer_reports([log, self.dir / "absent.log"])
        self.assertEqual([h.line for h in hits], [2, 3, 4])
        self.assertEqual({h.test for h in hits}, {None})

    def test_sanitizer_reports_thread_sanitizer(self) -> None:
        # TSan reports races as WARNINGs (the program still exits 66);
        # its closing tally and summary line are not new reports.
        log = self.log(
            "out.log",
            "==================\n"
            "WARNING: ThreadSanitizer: data race (pid=73502)\n"
            "  Write of size 8 at 0x00016bade7c8 by thread T3:\n"
            "SUMMARY: ThreadSanitizer: data race fastmutexTest.c:22 in fmWorker\n"
            "==3==ERROR: ThreadSanitizer: SEGV on unknown address\n"
            "ThreadSanitizer: reported 1 warnings\n",
        )
        hits = steps.sanitizer_reports([log])
        self.assertEqual([h.line for h in hits], [2, 5])

    def test_sanitizer_reports_name_their_test(self) -> None:
        long = "src/dod.c:723:26: runtime error: signed integer overflow: " + "9" * 200
        log = self.log("LastTest.log", LAST_TEST_LOG.format(long=long))
        hits = steps.sanitizer_reports([log])
        self.assertEqual(
            [(h.test, h.line) for h in hits],
            [("dod", 10), ("varint-dd", 22), ("varint-dd", 23)],
        )
        self.assertEqual(hits[0].text, long)
        self.assertEqual(hits[0].describe(), f"dod: {long} (LastTest.log:10)")
        self.assertEqual(steps.tests_with_reports(hits), ["dod", "varint-dd"])

    def test_parse_ctest_missing_executable(self) -> None:
        exe = "/w/build/deps/varint/src/varintPackedTest"
        log = self.log("noexe.log", CTEST_NO_EXE.format(bindir="/w/build", exe=exe))
        result = steps.parse_ctest(log)
        self.assertEqual(result.missing, {"varint-packed": exe})
        # Missing executables are not ordinary failures; a disabled
        # test's "Not Run" is not a missing executable.
        self.assertEqual(result.failed, [])
        self.assertEqual(result.not_run, ["skipme"])
        # A "Not Run" failure without the executable line stays a failure.
        other = self.log(
            "other.log",
            " 1/1 Test  #1: needsfile ...***Not Run   0.00 sec\n"
            "The following tests FAILED:\n\t  1 - needsfile (Not Run)\n",
        )
        self.assertEqual(steps.parse_ctest(other).failed, ["needsfile"])
        self.assertEqual(steps.parse_ctest(other).missing, {})

    def test_targets_for_tests(self) -> None:
        bindir = self.dir / "build"
        steps.request_codemodel(bindir)
        self.assertTrue((bindir / ".cmake/api/v1/query/codemodel-v2").is_file())
        self.assertEqual(steps.target_artifacts(bindir), {})
        write_codemodel(
            bindir,
            {
                "datakit-test": ["src/datakit-test"],
                "varintPackedTest": ["deps/varint/src/varintPackedTest"],
                "datakit-shared": ["src/libdatakit.dylib", "src/libdatakit.1.dylib"],
                "check": [],
            },
        )
        artifacts = steps.target_artifacts(bindir)
        real = os.path.realpath(bindir)
        self.assertEqual(artifacts[f"{real}/src/datakit-test"], "datakit-test")
        self.assertEqual(artifacts[f"{real}/src/libdatakit.1.dylib"], "datakit-shared")
        self.assertNotIn("check", artifacts.values())

        listing = self.log(
            "tests.json",
            "$ ctest --show-only=json-v1\n"
            + show_only_json(
                {"databox": ["x"], "varint-packed": [], "gate": [], "wrapped": []}
            ),
        )
        self.assertEqual(
            steps.parse_test_names(listing),
            ["databox", "varint-packed", "gate", "wrapped"],
        )
        self.assertEqual(steps.parse_test_names(self.log("bad.json", "$ x\n{")), [])
        self.assertEqual(steps.parse_test_names(self.dir / "absent.json"), [])

        commands = {
            "databox": [f"{bindir}/src/datakit-test", "test", "databox"],
            "varint-packed": [f"{bindir}/deps/varint/src/varintPackedTest"],
            "gate": ["/bin/sh", f"{bindir}/../tools/uvGate.sh", "src/x"],
            "wrapped": ["/bin/sh", "run.sh", f"{bindir}/src/datakit-test"],
            "bare": [],
        }
        out = self.dir / "commands.txt"
        out.write_text(commands_text(commands))
        self.assertEqual(steps.parse_test_commands(out), commands)
        self.assertEqual(steps.parse_test_commands(self.dir / "absent.txt"), {})
        self.assertEqual(
            steps.targets_for_tests(commands, artifacts),
            {
                "databox": ["datakit-test"],
                "varint-packed": ["varintPackedTest"],
                "wrapped": ["datakit-test"],
            },
        )

    @unittest.skipIf(shutil.which("cmake") is None, "cmake not on PATH")
    def test_ctest_commands_script(self) -> None:
        bindir = self.dir / "build"
        (bindir / "sub").mkdir(parents=True)
        (bindir / "CTestTestfile.cmake").write_text(
            'add_test(top "/b/top" "a b" "q\\"q" "back\\\\slash")\n'
            'set_tests_properties(top PROPERTIES  LABELS "gate")\n'
            'subdirs("sub")\n'
            'subdirs("missing")\n'
        )
        (bindir / "sub" / "CTestTestfile.cmake").write_text(
            'add_test([=[sub-test]=] "/b/sub/exe")\n'
            "add_test(none NOT_AVAILABLE)\n"
            "add_test(bare)\n"
        )
        out = self.dir / "commands.txt"
        proc = subprocess.run(
            steps.test_commands_command(bindir, out), capture_output=True, check=False
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(
            steps.parse_test_commands(out),
            {
                "top": ["/b/top", "a b", 'q"q', "back\\slash"],
                "sub-test": ["/b/sub/exe"],
                "none": ["NOT_AVAILABLE"],
                "bare": [],
            },
        )

    def test_ctest_command(self) -> None:
        cmd = steps.ctest_command(Path("b"), 8, "a|b", exclude=["c.*P", ""])
        self.assertIn("--no-tests=error", cmd)
        self.assertEqual(cmd[cmd.index("-R") + 1], "^(a|b)$")
        self.assertEqual(cmd[cmd.index("-E") + 1], "^(c.*P)$")
        self.assertNotIn("-E", steps.ctest_command(Path("b"), 1, "x", exclude=[""]))
        loaded = steps.ctest_command(Path("b"), 2, "x", load=8)
        self.assertEqual(loaded[loaded.index("--test-load") + 1], "8")
        self.assertNotIn("--test-load", cmd)

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

    def __init__(
        self,
        codes: dict[str, int],
        outputs: dict[str, str],
        codemodel: dict[str, list[str]] | None = None,
    ) -> None:
        self.codes = codes
        self.outputs = outputs
        self.codemodel = codemodel  # written by "configure", when given
        self.calls: list[tuple[str, list[str]]] = []
        self.timeouts: dict[str, float | None] = {}
        self.timed_out: set[str] = set()  # step names that "time out"

    def __call__(self, name, cmd, log, cwd=None, env=None, timeout=None) -> Step:
        self.calls.append((name, list(cmd)))
        self.timeouts[name] = timeout
        if name in self.timed_out:
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text(f"dktools: {name}: timed out after {timeout:g}s; killed\n")
            return Step(name, log, 124, timed_out=True)
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(self.outputs.get(name, ""))
        if name == "configure" and self.codemodel is not None:
            bindir = Path(cmd[cmd.index("-B") + 1])
            self.assertQueried(bindir)
            write_codemodel(bindir, self.codemodel)
        if name == "test-commands":
            out = next(a for a in cmd if a.startswith("-DOUT="))[len("-DOUT=") :]
            Path(out).write_text(self.outputs.get("test-commands-out", ""))
        if name.startswith("ctest"):
            binary_dir = Path(cmd[cmd.index("--test-dir") + 1])
            temp = binary_dir / "Testing" / "Temporary"
            temp.mkdir(parents=True, exist_ok=True)
            (temp / "LastTest.log").write_text(self.outputs.get(name + "-output", ""))
        return Step(name, log, self.codes.get(name, 0))

    @staticmethod
    def assertQueried(bindir: Path) -> None:
        if not (bindir / ".cmake/api/v1/query/codemodel-v2").is_file():
            raise AssertionError("configure ran without the codemodel query")

    def command(self, name: str) -> list[str]:
        return dict(self.calls)[name]


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
        self.assertEqual(
            names,
            [
                "configure",
                "ctest-list",
                "test-commands",
                "build",
                "ctest",
                "allocaudit",
                "atomicaudit",
                "outputaudit",
                "spell",
                "idioms",
                "magic",
                "modelcover",
                "headers",
                "testchecks",
                "docexamples",
            ],
        )
        self.assertIn("  spell     ok (", out)
        self.assertIn("  docs      ok (", out)
        self.assertIn("  magic     ok (", out)
        self.assertIn("  models    ok (", out)
        self.assertIn("  headers   ok (", out)
        self.assertIn("  idioms    ok (", out)
        self.assertIn("  checks    ok (", out)
        script = runner.command("test-commands")[-2:]
        self.assertEqual(script, ["-P", str(steps.CTEST_COMMANDS_SCRIPT)])
        self.assertTrue(steps.CTEST_COMMANDS_SCRIPT.is_file())
        listing = runner.command("ctest-list")
        self.assertIn("--show-only=json-v1", listing)
        self.assertEqual(listing[listing.index("-R") + 1], "^(databox)$")
        build = runner.command("build")
        self.assertEqual(build_targets(build), list(toolchain.PRESET_TARGETS))
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
            "atomic": ({"atomicaudit": 1}, {"ctest": CTEST_PASS}),
            "output": ({"outputaudit": 1}, {"ctest": CTEST_PASS}),
            "spell": ({"spell": 1}, {"ctest": CTEST_PASS}),
            "configure": ({"configure": 1}, {}),
        }
        for label, (codes, outputs) in cases.items():
            with self.subTest(label=label):
                code, out = self.run_verify(FakeRunner(codes, outputs))
                self.assertEqual(code, 1, out)
                self.assertIn("verify t: FAIL", out)
        code, out = self.run_verify(FakeRunner({"ctest": 8}, {"ctest": CTEST_FAIL}))
        self.assertIn("failed    flex multimapP", out)

    def test_every_step_has_a_timeout(self) -> None:
        runner = FakeRunner({}, {"ctest": CTEST_PASS, "allocaudit": "100% clean\n"})
        code, out = self.run_verify(runner, serial="flexP")
        self.assertEqual(code, 0, out)
        self.assertIn("ctest-serial", runner.timeouts)
        for name, timeout in runner.timeouts.items():
            with self.subTest(step=name):
                self.assertIsNotNone(timeout)
                self.assertGreater(timeout, 0)

    def test_timed_out_steps_fail_and_say_so(self) -> None:
        cases = {
            "configure": "build     FAILED (configure timed out)",
            "build": "build     FAILED (timed out)",
            "ctest": "ctest     FAILED (timed out;",
            "allocaudit": "alloc     FAILED (dktools: allocaudit: timed out after",
            "atomicaudit": "atomic    FAILED (dktools: atomicaudit: timed out after",
            "outputaudit": "output    FAILED (dktools: outputaudit: timed out after",
            "spell": "spell     FAILED (dktools: spell: timed out after",
        }
        for step, needle in cases.items():
            with self.subTest(step=step):
                runner = FakeRunner({}, {"ctest": CTEST_PASS})
                runner.timed_out.add(step)
                code, out = self.run_verify(runner)
                self.assertEqual(code, 1, out)
                self.assertIn(needle, out)

    def test_sanitizer_reports_print_test_and_full_line(self) -> None:
        long = "src/dod.c:723:26: runtime error: signed integer overflow: " + "7" * 200
        outputs = {
            "ctest": CTEST_PASS,
            "ctest-output": LAST_TEST_LOG.format(long=long),
            "allocaudit": "100% clean pattern\n",
        }
        code, out = self.run_verify(FakeRunner({}, outputs))
        self.assertEqual(code, 1, out)
        self.assertIn("sanitizer 3 report(s) from 2 test(s): dod varint-dd\n", out)
        self.assertIn(f"  report    dod: {long} (ctest-output.log:10)\n", out)
        self.assertIn(
            "  report    varint-dd: ==7==ERROR: AddressSanitizer: "
            "heap-buffer-overflow on address 0x1 (ctest-output.log:23)\n",
            out,
        )

    def bindir(self) -> Path:
        return self.root.resolve() / "verify" / "t" / "src" / "build-clang22-dev"

    def test_builds_the_targets_the_selected_tests_run(self) -> None:
        b = self.bindir()
        runner = FakeRunner(
            {},
            {
                # ctest's JSON omits the commands of unbuilt executables.
                "ctest-list": show_only_json(
                    {
                        "databox": [],
                        "varint-packed": [],
                        "varint-dd": [],
                        "allocOwnership": [],
                    }
                ),
                "test-commands-out": commands_text(
                    {
                        "databox": [f"{b}/src/datakit-test", "test", "databox"],
                        "varint-packed": [f"{b}/deps/varint/src/varintPackedTest"],
                        "varint-dd": [f"{b}/deps/varint/src/varintDDTest", "1"],
                        "varint-other": [f"{b}/deps/varint/src/varintOtherTest"],
                        "allocOwnership": ["/bin/sh", "uvGate.sh"],
                    }
                ),
                "ctest": CTEST_PASS,
                "ctest-serial": CTEST_PASS,
                "allocaudit": "100% clean pattern\n",
            },
            codemodel={
                "datakit-test": ["src/datakit-test"],
                "varintPackedTest": ["deps/varint/src/varintPackedTest"],
                "varintDDTest": ["deps/varint/src/varintDDTest"],
                "varintOtherTest": ["deps/varint/src/varintOtherTest"],
            },
        )
        code, out = self.run_verify(runner, tests="databox|varint-.*", serial="dd")
        self.assertEqual(code, 0, out)
        listing = runner.command("ctest-list")
        self.assertEqual(listing[listing.index("-R") + 1], "^(databox|varint-.*|dd)$")
        build = runner.command("build")
        self.assertEqual(
            build_targets(build),
            [*toolchain.PRESET_TARGETS, "varintPackedTest", "varintDDTest"],
        )
        self.assertIn(
            "  targets   datakit-test datakit-static datakit-shared datakit-library"
            " + 2 for the selected tests: varintPackedTest varintDDTest\n",
            out,
        )

    def test_missing_executables_are_their_own_error(self) -> None:
        b = self.bindir()
        exe = f"{b}/deps/varint/src/varintPackedTest"
        runner = FakeRunner(
            {"ctest": 8},
            {
                "ctest": CTEST_NO_EXE.format(bindir=b, exe=exe),
                "allocaudit": "100% clean pattern\n",
            },
            codemodel={"varintPackedTest": ["deps/varint/src/varintPackedTest"]},
        )
        code, out = self.run_verify(runner, targets=["datakit-test"])
        self.assertEqual(code, 1, out)
        names = [name for name, _ in runner.calls]
        self.assertNotIn("ctest-list", names)
        build = runner.command("build")
        self.assertEqual(build_targets(build), ["datakit-test"])
        self.assertIn("  targets   datakit-test (--target)\n", out)
        self.assertIn(
            "  missing   1 test(s) did not run, executable not built: varint-packed\n",
            out,
        )
        self.assertIn(
            "  hint      not built: varintPackedTest; "
            "add them with --target, or use --target all\n",
            out,
        )
        self.assertNotIn("  failed ", out)

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

    PASSING = {"ctest": CTEST_PASS, "allocaudit": "100% clean pattern\n"}

    def exported(self, rel: str) -> Path:
        return self.root / "verify" / "t" / "src" / rel

    def test_staged_builds_exactly_the_index(self) -> None:
        self.repo.write("src/a.c", "int staged;\n")
        self.repo.write("src/new.c", "int added;\n")
        self.repo.git("add", "--", "src/a.c", "src/new.c")
        self.repo.write("src/a.c", "int unstaged;\n")
        self.repo.write("src/stray.c", "int stray;\n")
        index = self.repo.path / ".git" / "index"
        before = index.read_bytes()
        code, out = self.run_verify(FakeRunner({}, self.PASSING), specs=[], staged=True)
        self.assertEqual(code, 0, out)
        self.assertEqual(index.read_bytes(), before)
        self.assertEqual(self.exported("src/a.c").read_text(), "int staged;\n")
        self.assertTrue(self.exported("src/new.c").exists())
        self.assertFalse(self.exported("src/stray.c").exists())
        self.assertIn("(HEAD ", out)
        self.assertIn(" + the index, 2 changed)\n", out)
        self.assertIn("  changed   M src/a.c\n  changed   A src/new.c\n", out)

    def test_all_changed_builds_every_changed_path_and_new_source(self) -> None:
        self.repo.write("src/gone.c", "int gone;\n")
        self.repo.write("src/same.c", "int same;\n")
        self.repo.commit("more", "src/gone.c", "src/same.c")
        self.repo.write("src/a.c", "int worktree;\n")
        (self.repo.path / "src" / "gone.c").unlink()
        self.repo.write("src/added.c", "int added;\n")
        self.repo.git("add", "--", "src/added.c")
        # An untracked file under src/ is part of the change (a new
        # header a changed file includes); one elsewhere is not
        self.repo.write("src/fresh.h", "int fresh;\n")
        self.repo.write("scratch/draft.md", "draft\n")
        code, out = self.run_verify(
            FakeRunner({}, self.PASSING), specs=[], all_changed=True
        )
        self.assertEqual(code, 0, out)
        self.assertEqual(self.exported("src/a.c").read_text(), "int worktree;\n")
        self.assertTrue(self.exported("src/added.c").exists())
        self.assertTrue(self.exported("src/same.c").exists())
        self.assertTrue(self.exported("src/fresh.h").exists())
        self.assertFalse(self.exported("src/gone.c").exists())
        self.assertFalse(self.exported("scratch/draft.md").exists())
        self.assertIn(" + all changed paths, 4 changed)\n", out)
        for line in ("M src/a.c", "A src/added.c", "A src/fresh.h", "D src/gone.c"):
            self.assertIn(f"  changed   {line}\n", out)
        self.assertNotIn("same.c", out)

    def test_named_paths_are_formatted_in_place_first(self) -> None:
        self.repo.write("src/a.c", "int  a2;\n")  # LLVM style: one space
        self.repo.write("src/other.c", "int  other;\n")  # not named: untouched
        code, out = self.run_verify(FakeRunner({}, self.PASSING))
        self.assertEqual(code, 0, out)
        self.assertEqual((self.repo.path / "src" / "a.c").read_text(), "int a2;\n")
        self.assertEqual(self.exported("src/a.c").read_text(), "int a2;\n")
        self.assertEqual(
            (self.repo.path / "src" / "other.c").read_text(), "int  other;\n"
        )
        self.assertIn("  format    ok (1 files)\n", out)
        self.assertIn("  formatted clang-format: rewrote 1: src/a.c\n", out)

    def test_staged_is_checked_never_rewritten(self) -> None:
        self.repo.write("src/a.c", "int  staged;\n")
        self.repo.git("add", "--", "src/a.c")
        code, out = self.run_verify(FakeRunner({}, self.PASSING), specs=[], staged=True)
        self.assertEqual(code, 1, out)
        self.assertIn("verify t: FAIL", out)
        self.assertIn("  format    FAILED (1 files; ", out)
        self.assertNotIn("formatted", out)
        self.assertEqual((self.repo.path / "src" / "a.c").read_text(), "int  staged;\n")

    def test_staged_and_all_changed_refuse_plain_head(self) -> None:
        self.repo.write("scratch/stray.md", "stray\n")  # outside src/: not a change
        for kw, why in (
            ({"staged": True}, "--staged: index matches HEAD, nothing to verify"),
            ({"all_changed": True}, "--all-changed: no changes, nothing"),
        ):
            with self.subTest(kw=kw), self.assertRaisesRegex(ToolError, f"^{why}"):
                self.run_verify(FakeRunner({}, self.PASSING), specs=[], **kw)
        # An unchanged PATH builds plain HEAD on purpose.
        code, out = self.run_verify(FakeRunner({}, self.PASSING), specs=["src/a.c"])
        self.assertEqual(code, 0, out)
        self.assertIn(" + 1 path(s), 0 changed)\n", out)

    def test_refused_runs_keep_the_previous_result(self) -> None:
        self.repo.write("src/a.c", "int changed;\n")
        code, out = self.run_verify(FakeRunner({}, self.PASSING))
        self.assertEqual(code, 0, out)
        work = self.root / "verify" / "t"
        kept = {p: p.read_bytes() for p in work.rglob("*") if p.is_file()}
        self.assertIn(work / "result.txt", kept)
        self.repo.write("src/a.c", "int a;\n")  # back to HEAD: nothing changed
        for kw in (
            {"specs": [], "staged": True},
            {"specs": [], "all_changed": True},
            {"specs": ["src/missing.c"]},
        ):
            with self.subTest(kw=kw):
                runner = FakeRunner({}, self.PASSING)
                with self.assertRaises(ToolError):
                    self.run_verify(runner, **kw)
                self.assertEqual(runner.calls, [])
                now = {p: p.read_bytes() for p in work.rglob("*") if p.is_file()}
                self.assertEqual(now, kept)

    def gcc(self) -> toolchain.Compiler:
        exe = sys.executable
        return toolchain.Compiler("gcc15", exe, exe, None, None)

    def test_also_build_compiles_the_same_tree_without_tests(self) -> None:
        self.repo.write("src/a.c", "int changed;\n")
        runner = FakeRunner({}, self.PASSING)
        code, out = self.run_verify(runner, also_build=[self.gcc()])
        self.assertEqual(code, 0, out)
        names = [call[0] for call in runner.calls]
        self.assertIn("gcc15-release configure", names)
        self.assertIn("gcc15-release build", names)
        self.assertEqual(names.count("ctest"), 1)  # tests on the main compiler only
        self.assertIn("  also      gcc15-release build ok, 0 warning lines\n", out)
        self.assertRegex(out, r"\n  time      .*total")

    def test_also_build_failure_or_warning_fails_the_run(self) -> None:
        self.repo.write("src/a.c", "int changed;\n")
        for codes, outputs, line in (
            ({"gcc15-release build": 2}, {}, "FAILED (exit 2)"),
            (
                {},
                {"gcc15-release build": "a.c:1:1: warning: unused [-Wx]\n"},
                "1 warning",
            ),
        ):
            with self.subTest(line=line):
                runner = FakeRunner(codes, {**self.PASSING, **outputs})
                code, out = self.run_verify(runner, also_build=[self.gcc()])
                self.assertEqual(code, 1, out)
                self.assertIn(line, out)

    def test_named_targets_without_tests_build_only(self) -> None:
        self.repo.write("src/a.c", "int changed;\n")
        with self.assertRaisesRegex(ToolError, "--tests, --serial or --target"):
            self.run_verify(FakeRunner({}, self.PASSING), tests=None)
        runner = FakeRunner({}, self.PASSING)
        code, out = self.run_verify(runner, tests=None, targets=["datakit-static"])
        self.assertEqual(code, 0, out)
        self.assertIn("  ctest     -\n", out)
        self.assertNotIn("ctest", [call[0] for call in runner.calls])

    def test_source_flags_conflict(self) -> None:
        for kw in (
            {"staged": True},
            {"all_changed": True},
            {"specs": [], "staged": True, "all_changed": True},
            {"specs": []},
        ):
            with self.subTest(kw=kw), self.assertRaises(ToolError):
                self.run_verify(FakeRunner({}, self.PASSING), **kw)

    def test_cli_staged_with_paths_is_an_input_error(self) -> None:
        err = io.StringIO()
        argv = ["--repo", str(self.repo.path), "verify", "t", "--tests", "x"]
        argv += ["--root", str(self.root), "--staged", "src/a.c"]
        with contextlib.redirect_stderr(err):
            code = cli.main(argv)
        self.assertEqual(code, 2)
        self.assertEqual(
            err.getvalue().strip(),
            "dktools verify: --staged takes no PATH arguments (got src/a.c)",
        )

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
