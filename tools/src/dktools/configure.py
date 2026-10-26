"""``dktools configure``: (re)configure a preset build in place.

Runs ``cmake --preset PRESET`` in the repository with the named
compiler, so ``build/PRESET`` (and its ``compile_commands.json``, the
database clangd reads) follows the current CMake files: after a change
of flags, standard or compiler the editor sees what the build compiles.
It configures only; ``dktools verify`` / ``validate`` do the building.

Exits 0 when cmake configured the build, 1 when it failed (its output
is printed), 2 for a preset the compiler does not build.
"""

from __future__ import annotations

import sys
from pathlib import Path

from . import procs, toolchain
from .gitutil import ToolError

#: configure stops after this long (a fresh configure takes seconds)
CONFIGURE_TIMEOUT = 600.0


def configure_command(compiler: toolchain.Compiler, preset: str) -> list[str]:
    """The cmake argv configuring ``build/<preset>`` with `compiler`."""
    config = toolchain.build_config(compiler, preset)
    cmake_preset = toolchain.PRESETS[config.preset]
    if cmake_preset is None:
        raise ToolError(f"{preset}: not a cmake preset (it has no build/{preset})")
    return ["cmake", "--preset", cmake_preset, *compiler.cmake_defines()]


def run(repo: Path, compiler: toolchain.Compiler, preset: str) -> int:
    cmd = configure_command(compiler, preset)
    compiler.check()
    proc = procs.run(cmd, cwd=repo, timeout=CONFIGURE_TIMEOUT)
    if proc.returncode != 0:
        sys.stdout.write(proc.stdout.decode(errors="replace"))
        sys.stdout.write(proc.stderr.decode(errors="replace"))
        print(f"configure {preset}: FAIL (cmake exit {proc.returncode})")
        return 1
    print(
        f"configure {preset}: ok ({compiler.name}; "
        f"build/{preset}/compile_commands.json is current)"
    )
    return 0
