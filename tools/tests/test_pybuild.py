"""``dktools pybuild`` / ``pytest``: the commands they run and how they read results.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

from dktools import pybuild, toolchain


class Commands(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = Path("repo")
        self.clang = toolchain.resolve_compiler("clang22", env={})

    def test_the_wheel_builds_with_the_locked_uv_and_the_named_compiler(self) -> None:
        cmd = pybuild.build_command(self.repo, "n", self.clang)
        self.assertEqual(cmd[:4], [sys.executable, "-m", "uv", "build"])
        # the free-threaded interpreter builds the one abi3t binary
        self.assertEqual(cmd[cmd.index("--python") + 1], "3.15t")
        self.assertIn("--managed-python", cmd)
        self.assertEqual(
            cmd[cmd.index("--build-constraints") + 1],
            str(self.repo / "python" / "build-constraints.txt"),
        )
        out = self.repo / "build" / "dktools" / "pybuild" / "n"
        self.assertEqual(cmd[cmd.index("--out-dir") + 1], str(out / "wheel"))
        self.assertIn(f"build-dir={out / 'cmake'}", cmd)
        for define in (
            f"cmake.define.CMAKE_C_COMPILER={self.clang.cc}",
            f"cmake.define.CMAKE_AR={self.clang.ar}",
            f"cmake.define.CMAKE_RANLIB={self.clang.ranlib}",
        ):
            self.assertIn(define, cmd)
        self.assertEqual(cmd[-1], str(self.repo / "python"))

    def test_gcc_passes_no_archiver(self) -> None:
        gcc = toolchain.resolve_compiler("gcc15", env={})
        cmd = pybuild.build_command(self.repo, "n", gcc)
        self.assertFalse([a for a in cmd if "CMAKE_AR" in a or "CMAKE_RANLIB" in a])

    def test_each_interpreter_gets_a_fresh_environment_and_the_installed_wheel(
        self,
    ) -> None:
        wheel = Path("w.whl")
        venv_cmd, install, run = pybuild.test_commands(
            self.repo, "n", "3.15t", wheel, "test*.py"
        )
        venv = pybuild.venv_dir(self.repo, "n", "3.15t")
        self.assertEqual(venv.name, "venv-3.15t")
        self.assertEqual(
            venv_cmd[3:],
            ["venv", "--managed-python", "--python", "3.15t", "--clear", str(venv)],
        )
        self.assertIn(str(wheel), install)
        self.assertIn("--reinstall", install)
        python = str(venv / "bin" / "python")
        self.assertEqual(install[install.index("--python") + 1], python)
        self.assertEqual(run[:4], [python, "-m", "unittest", "discover"])
        tests = str(self.repo / "python" / "tests")
        # top-level dir is the tests dir: the source package is never on the path
        self.assertEqual(run[run.index("-t") + 1], tests)
        self.assertEqual(run[run.index("-p") + 1], "test*.py")


class Results(unittest.TestCase):
    def test_one_built_wheel_is_found_and_none_or_two_are_not(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            repo = Path(d)
            wheels = pybuild.work_dir(repo, "n") / "wheel"
            self.assertIsNone(pybuild.built_wheel(repo, "n"))
            wheels.mkdir(parents=True)
            (wheels / "datakit-0.1.0-cp315-abi3.abi3t-x.whl").write_bytes(b"")
            self.assertEqual(
                pybuild.built_wheel(repo, "n").name,
                "datakit-0.1.0-cp315-abi3.abi3t-x.whl",
            )
            (wheels / "datakit-0.1.0-other.whl").write_bytes(b"")
            self.assertIsNone(pybuild.built_wheel(repo, "n"))

    def test_the_summary_is_unittests_closing_lines(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / "log"
            log.write_text("...\nRan 17 tests in 0.03s\n\nOK (skipped=1)\n")
            self.assertEqual(
                pybuild._unittest_summary(log), "Ran 17 tests in 0.03s, OK (skipped=1)"
            )
            log.write_text("ImportError: no module\n")
            self.assertEqual(pybuild._unittest_summary(log), "no tests ran")

    def test_testing_with_no_wheel_exits_2(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(pybuild.run_tests(Path(d), "n", ("3.15",), "t*.py"), 2)


if __name__ == "__main__":
    unittest.main()
