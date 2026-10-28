"""``dktools pybuild`` / ``dktools pytest``: the Python package's wheel and tests.

``dktools pybuild NAME [--compiler clang22]`` builds the ``python/``
package (the ``datakit`` module over its C extension, one ``abi3t`` wheel
for the regular and the free-threaded interpreters) into
``build/dktools/pybuild/NAME/wheel``, its CMake tree in ``.../cmake``,
the build's output in ``.../build.log``.  The build backend's versions
are pinned by ``python/build-constraints.txt``.

``dktools pytest NAME [--python 3.15,3.15t] [--tests PATTERN]`` installs
that wheel into a fresh virtual environment per interpreter
(``.../venv-<python>``, uv-managed interpreters only) and runs the
package's tests (``python/tests``, stdlib ``unittest``) from outside the
source tree, so they import the installed wheel.  One result line per
interpreter; each stage's output in ``.../test-<python>-<stage>.log``
(stages ``venv``, ``install``, ``tests``).  ``dktools validate
--python`` runs both on the exported tree.

Both exit 0 on success, 1 on a failed build or test run, 2 when the
wheel to test is missing.
"""

from __future__ import annotations

import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from . import steps, toolchain
from .steps import Step

#: the interpreters the wheel is tested on: regular and free-threaded
DEFAULT_PYTHONS = ("3.15", "3.15t")
#: the interpreter that builds it: the build backend makes the abi3t
#: binary (one for both kinds) only under a free-threaded interpreter
BUILD_PYTHON = "3.15t"

#: a wheel build compiles all of datakit (minutes on a cold tree)
BUILD_TIMEOUT = 1800.0
#: installing a local wheel plus running the package's tests
TEST_TIMEOUT = 600.0


def work_dir(repo: Path, name: str) -> Path:
    return repo / "build" / "dktools" / "pybuild" / name


def uv(*args: str) -> list[str]:
    """argv running the locked uv (tools' own dependency)."""
    return [sys.executable, "-m", "uv", *args]


def build_command(repo: Path, name: str, compiler: toolchain.Compiler) -> list[str]:
    """argv building the wheel with `compiler`, from a managed 3.15."""
    out = work_dir(repo, name)
    defines = {"CMAKE_C_COMPILER": compiler.cc}
    if compiler.ar is not None:
        defines["CMAKE_AR"] = compiler.ar
    if compiler.ranlib is not None:
        defines["CMAKE_RANLIB"] = compiler.ranlib
    cmd = uv(
        "build",
        "--wheel",
        "--managed-python",
        "--python",
        BUILD_PYTHON,
        "--build-constraints",
        str(repo / "python" / "build-constraints.txt"),
        "--out-dir",
        str(out / "wheel"),
        "-C",
        f"build-dir={out / 'cmake'}",
    )
    for key, value in defines.items():
        cmd += ["-C", f"cmake.define.{key}={value}"]
    cmd.append(str(repo / "python"))
    return cmd


def venv_dir(repo: Path, name: str, python: str) -> Path:
    return work_dir(repo, name) / f"venv-{python}"


def test_commands(
    repo: Path, name: str, python: str, wheel: Path, pattern: str
) -> list[list[str]]:
    """argvs making `python`'s environment, installing `wheel` and running
    the tests whose file names match `pattern`."""
    venv = venv_dir(repo, name, python)
    interpreter = str(venv / "bin" / "python")
    tests = str(repo / "python" / "tests")
    return [
        uv("venv", "--managed-python", "--python", python, "--clear", str(venv)),
        uv("pip", "install", "--python", interpreter, "--reinstall", str(wheel)),
        [
            interpreter,
            "-m",
            "unittest",
            "discover",
            "-s",
            tests,
            "-t",
            tests,
            "-p",
            pattern,
        ],
    ]


def built_wheel(repo: Path, name: str) -> Path | None:
    """The one wheel pybuild NAME made, or None."""
    wheels = sorted((work_dir(repo, name) / "wheel").glob("datakit-*.whl"))
    return wheels[-1] if len(wheels) == 1 else None


@dataclass
class Built:
    """A wheel build: its step, and the wheel when it made exactly one."""

    step: Step
    wheel: Path | None

    @property
    def ok(self) -> bool:
        return self.step.ok and self.wheel is not None


@dataclass
class Tested:
    """One interpreter's run: the step that ended it and unittest's verdict."""

    python: str
    step: Step
    summary: str

    @property
    def ok(self) -> bool:
        return self.step.ok


def build(repo: Path, name: str, compiler: toolchain.Compiler) -> Built:
    """Build the wheel (any earlier one removed first)."""
    out = work_dir(repo, name)
    wheel_dir = out / "wheel"
    wheel_dir.mkdir(parents=True, exist_ok=True)
    for old in wheel_dir.glob("*.whl"):
        old.unlink()
    step = steps.run_logged(
        "python build",
        build_command(repo, name, compiler),
        out / "build.log",
        cwd=out,
        timeout=BUILD_TIMEOUT,
    )
    return Built(step, built_wheel(repo, name))


#: the stages of one interpreter's run, in test_commands' order
TEST_STAGES = ("venv", "install", "tests")


def wheel_tests(
    repo: Path, name: str, python: str, wheel: Path, pattern: str
) -> Tested:
    """Run `python`'s stages until one fails; the tests' log gives the summary."""
    out = work_dir(repo, name)
    step = Step(f"python {python}", out / f"test-{python}-venv.log")
    for stage, cmd in zip(
        TEST_STAGES, test_commands(repo, name, python, wheel, pattern), strict=True
    ):
        step = steps.run_logged(
            f"python {python} {stage}",
            cmd,
            out / f"test-{python}-{stage}.log",
            cwd=out,
            timeout=TEST_TIMEOUT,
        )
        if not step.ok:
            break
    summary = _unittest_summary(step.log) if step.log.exists() else "no tests ran"
    return Tested(python, step, summary)


def describe(tested: Tested) -> str:
    """One result line's body: ok/FAIL, the verdict, the log on failure."""
    if tested.ok:
        return f"ok ({tested.summary})"
    return f"FAIL ({tested.step.name}: {tested.summary}; see {tested.step.log})"


def run_build(repo: Path, name: str, compiler: toolchain.Compiler) -> int:
    compiler.check()
    built = build(repo, name, compiler)
    if not built.ok:
        print(f"pybuild {name}: FAIL (exit {built.step.code}; see {built.step.log})")
        return 1
    assert built.wheel is not None
    print(f"pybuild {name}: ok ({compiler.name}) {built.wheel.name}")
    return 0


def run_tests(repo: Path, name: str, pythons: Sequence[str], pattern: str) -> int:
    wheel = built_wheel(repo, name)
    if wheel is None:
        print(f"pytest {name}: no wheel (run dktools pybuild {name} first)")
        return 2
    failed = 0
    for python in pythons:
        tested = wheel_tests(repo, name, python, wheel, pattern)
        failed += not tested.ok
        print(f"pytest {name} {python}: {describe(tested)}")
    return 1 if failed else 0


def _unittest_summary(log: Path) -> str:
    """unittest's closing 'Ran N tests' and verdict lines, joined."""
    lines = log.read_text(errors="replace").splitlines()
    ran = next((ln for ln in reversed(lines) if ln.startswith("Ran ")), "no tests ran")
    verdict = next(
        (ln for ln in reversed(lines) if ln.startswith(("OK", "FAILED"))), ""
    )
    return f"{ran}, {verdict}" if verdict else ran
