"""Named compilers and build configurations for datakit builds.

The single place that knows which compilers datakit is verified with
and the CMake flags each needs.  ``verify`` and ``validate`` both build
through it, so a build never silently falls back to the system
``/usr/bin/cc``: the default compiler is ``clang22``, Homebrew LLVM
clang 22 with LLVM's own archiver (Apple's ``ar`` cannot index LLVM 22
bitcode).

Compilers (``COMPILERS``):

  clang22  C=clang-22, CXX=clang++, AR=llvm-ar, RANLIB=llvm-ranlib, all
           from one LLVM bin directory (default
           ``/opt/homebrew/opt/llvm/bin``; override with ``--llvm-dir``
           or ``$DKTOOLS_LLVM_DIR``).
  gcc15    C=gcc-15, CXX=g++-15 from PATH (override with ``--gcc-cc`` /
           ``--gcc-cxx`` or ``$DKTOOLS_GCC_CC`` / ``$DKTOOLS_GCC_CXX``).

Build presets (``PRESETS``):

  dev      ``cmake --preset dev`` (Release-O3 + ASan/UBSan), clang22.
  debug    ``cmake --preset debug`` (Debug + ASan/UBSan), clang22.
  tsan     ``cmake --preset tsan`` (RelWithDebInfo + ThreadSanitizer),
           clang22; opt-in, for data races.
  perf     ``cmake --preset perf`` (Release -O3 -mcpu=native -flto, no
           sanitizers), clang22; opt-in, for benchmark numbers.
  coverage ``cmake --preset coverage`` (Release + clang source coverage,
           no sanitizers), clang22; what ``dktools coverage`` builds.
  release  no preset, ``-DCMAKE_BUILD_TYPE=Release``; the only build
           gcc15 runs (GCC on macOS has no sanitizer runtimes).

Flags beat environment variables, which beat the defaults.
"""

from __future__ import annotations

import argparse
import json
import os
import re
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path

from .gitutil import ToolError, require_program

DEFAULT_LLVM_DIR = "/opt/homebrew/opt/llvm/bin"
ENV_LLVM_DIR = "DKTOOLS_LLVM_DIR"
DEFAULT_GCC_CC = "gcc-15"
DEFAULT_GCC_CXX = "g++-15"
ENV_GCC_CC = "DKTOOLS_GCC_CC"
ENV_GCC_CXX = "DKTOOLS_GCC_CXX"

DEFAULT_COMPILER = "clang22"

#: The targets every buildPreset in CMakePresets.json builds; a
#: targeted build that skips any of them leaves a library stale.
PRESET_TARGETS = ("datakit-test", "datakit-static", "datakit-shared", "datakit-library")

#: preset name -> CMakePresets.json configure preset, or None for a
#: plain ``-DCMAKE_BUILD_TYPE=Release`` configure without a preset.
PRESETS: dict[str, str | None] = {
    "dev": "dev",
    "debug": "debug",
    "tsan": "tsan",
    "perf": "perf",
    "coverage": "coverage",
    "release": None,
}

#: compiler name -> the presets it builds, default first.
COMPILER_PRESETS: dict[str, tuple[str, ...]] = {
    "clang22": ("dev", "debug", "tsan", "perf", "coverage"),
    "gcc15": ("release",),
}

COMPILERS = tuple(COMPILER_PRESETS)


@dataclass(frozen=True)
class Compiler:
    """One named compiler: the programs and the cmake -D flags they need."""

    name: str
    cc: str
    cxx: str
    ar: str | None = None
    ranlib: str | None = None

    def cmake_defines(self) -> list[str]:
        """The ``-D`` flags selecting this compiler (and its archiver)."""
        flags = [f"-DCMAKE_C_COMPILER={self.cc}", f"-DCMAKE_CXX_COMPILER={self.cxx}"]
        if self.ar is not None:
            flags.append(f"-DCMAKE_AR={self.ar}")
        if self.ranlib is not None:
            flags.append(f"-DCMAKE_RANLIB={self.ranlib}")
        return flags

    def check(self) -> None:
        """ToolError unless every program of this compiler exists."""
        for program in (self.cc, self.cxx, self.ar, self.ranlib):
            if program is not None:
                try:
                    require_program(program)
                except ToolError as exc:
                    raise ToolError(f"compiler {self.name}: {exc}") from exc


def resolve_compiler(
    name: str,
    llvm_dir: str | None = None,
    gcc_cc: str | None = None,
    gcc_cxx: str | None = None,
    env: Mapping[str, str] | None = None,
) -> Compiler:
    """The Compiler called `name`, with flag/env/default overrides."""
    environ = os.environ if env is None else env
    if name == "clang22":
        bindir = Path(llvm_dir or environ.get(ENV_LLVM_DIR) or DEFAULT_LLVM_DIR)
        return Compiler(
            name=name,
            cc=str(bindir / "clang-22"),
            cxx=str(bindir / "clang++"),
            ar=str(bindir / "llvm-ar"),
            ranlib=str(bindir / "llvm-ranlib"),
        )
    if name == "gcc15":
        return Compiler(
            name=name,
            cc=gcc_cc or environ.get(ENV_GCC_CC) or DEFAULT_GCC_CC,
            cxx=gcc_cxx or environ.get(ENV_GCC_CXX) or DEFAULT_GCC_CXX,
        )
    raise ToolError(f"unknown compiler {name!r} (known: {', '.join(COMPILERS)})")


@dataclass(frozen=True)
class BuildConfig:
    """A (compiler, preset) pair and the commands that build it."""

    compiler: Compiler
    preset: str
    #: extra cmake cache entries, NAME=VALUE, after the preset's own
    defines: tuple[str, ...] = ()

    @property
    def label(self) -> str:
        base = f"{self.compiler.name}-{self.preset}"
        return "-".join([base, *self.defines]) if self.defines else base

    @property
    def runs_tests(self) -> bool:
        """False for a build without the test binary (BuildTestBinary=OFF):
        it builds and is judged on its warnings, with no ctest."""
        return "BuildTestBinary=OFF" not in self.defines

    def binary_dir(self, source: Path) -> Path:
        return source / f"build-{self.label}"

    def configure_command(self, source: Path) -> list[str]:
        """``cmake`` configure argv for `source` (fresh binary dir)."""
        cmd = ["cmake", "-S", str(source), "-B", str(self.binary_dir(source))]
        configure_preset = PRESETS[self.preset]
        if configure_preset is not None:
            cmd += ["--preset", configure_preset]
        else:
            cmd += ["-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"]
        return cmd + self.compiler.cmake_defines() + [f"-D{d}" for d in self.defines]

    def build_command(
        self,
        source: Path,
        jobs: int,
        targets: Sequence[str] | None,
        load: int | None = None,
        keep_going: bool = False,
    ) -> list[str]:
        """``cmake --build`` argv; `targets` None builds all targets; with
        `load`, make starts no new job while the load average is at or
        above it; with `keep_going`, make builds everything it can past a
        failed compile, so one build shows every error."""
        cmd = ["cmake", "--build", str(self.binary_dir(source)), "-j", str(jobs)]
        if targets:
            cmd += ["--target", *targets]
        make_args = (["-l", str(load)] if load else []) + (["-k"] if keep_going else [])
        if make_args:
            cmd += ["--", *make_args]
        return cmd

    def check_presets_file(self, source: Path) -> None:
        """ToolError unless `source`'s CMakePresets.json defines the preset."""
        configure_preset = PRESETS[self.preset]
        if configure_preset is None:
            return
        presets_file = source / "CMakePresets.json"
        try:
            data = json.loads(presets_file.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise ToolError(f"{presets_file}: cannot read presets: {exc}") from exc
        names = {p.get("name") for p in data.get("configurePresets", [])}
        if configure_preset not in names:
            raise ToolError(f"{presets_file}: no configure preset {configure_preset!r}")


#: a cmake cache entry a caller may add: NAME=VALUE, both plain words
DEFINE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*=[A-Za-z0-9_.+-]+")

#: The library-only build: no DATAKIT_TEST code and no test binary, as a
#: consumer that turns test mode off compiles datakit.
LIBRARY_ONLY_DEFINES = ("DatakitTestMode=OFF", "BuildTestBinary=OFF")


def build_config(
    compiler: Compiler, preset: str | None, defines: tuple[str, ...] = ()
) -> BuildConfig:
    """Pair `compiler` with `preset` (None: the compiler's default), plus
    any extra cmake `defines` (NAME=VALUE)."""
    allowed = COMPILER_PRESETS[compiler.name]
    chosen = preset or allowed[0]
    if chosen not in allowed:
        raise ToolError(
            f"compiler {compiler.name} builds preset(s) {', '.join(allowed)}, "
            f"not {chosen!r}"
        )
    for define in defines:
        if not DEFINE.fullmatch(define):
            raise ToolError(f"--define {define!r}: not NAME=VALUE")
    return BuildConfig(compiler, chosen, tuple(defines))


def matrix(
    compilers: Sequence[Compiler], presets: Sequence[str], library_only: bool = False
) -> list[BuildConfig]:
    """Every (compiler, preset) a validate run builds.

    Each compiler builds the requested presets it supports; a compiler
    that supports none of them (gcc15 builds only ``release``) builds
    its default.  With `library_only`, each compiler also builds its
    default preset with LIBRARY_ONLY_DEFINES (after its tested configs)."""
    for preset in presets:
        if preset not in PRESETS:
            raise ToolError(f"unknown preset {preset!r} (known: {', '.join(PRESETS)})")
    configs: list[BuildConfig] = []
    for compiler in compilers:
        allowed = COMPILER_PRESETS[compiler.name]
        chosen = [p for p in presets if p in allowed] or [allowed[0]]
        configs.extend(BuildConfig(compiler, p) for p in chosen)
    if library_only:
        configs.extend(
            BuildConfig(c, COMPILER_PRESETS[c.name][0], LIBRARY_ONLY_DEFINES)
            for c in compilers
        )
    return configs


def add_arguments(parser: argparse.ArgumentParser) -> None:
    """The compiler-location flags shared by verify and validate."""
    parser.add_argument(
        "--llvm-dir",
        default=None,
        help=f"clang22 bin directory (env {ENV_LLVM_DIR}, default {DEFAULT_LLVM_DIR})",
    )
    parser.add_argument(
        "--gcc-cc",
        default=None,
        help=f"gcc15 C compiler (env {ENV_GCC_CC}, default {DEFAULT_GCC_CC})",
    )
    parser.add_argument(
        "--gcc-cxx",
        default=None,
        help=f"gcc15 C++ compiler (env {ENV_GCC_CXX}, default {DEFAULT_GCC_CXX})",
    )


def from_args(name: str, args: argparse.Namespace) -> Compiler:
    """resolve_compiler with the flags add_arguments registered."""
    return resolve_compiler(
        name, llvm_dir=args.llvm_dir, gcc_cc=args.gcc_cc, gcc_cxx=args.gcc_cxx
    )
