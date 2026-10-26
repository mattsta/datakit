"""What the named compilers and their libc do with C23 features.

``dktools c23probe [--compiler NAME ...]`` compiles a fixed set of small
programs with each compiler (``-std=gnu23 -O2``) and prints one row per
probe and one column per compiler.  It answers the questions the C23
program asks before it adopts a feature, and re-answers them after every
toolchain update:

- ``calls`` probes compile to assembly and count the calls to ``f`` in
  ``f(x) + f(x)``.  One call means the compiler folds the second call
  away (it trusts the attribute); two mean it accepts the attribute but
  does not optimize on it.  The ``gnu::`` rows are the baseline the
  standard attributes are measured against.
- ``compile`` probes say whether the compiler accepts the feature
  without a diagnostic.
- ``run`` probes compile, link and run a program and compare what it
  prints: library support (a printf length modifier, a libc function)
  that compiling alone cannot show.

A cell reads ``folds`` / ``no fold`` (calls), ``yes`` (compile), ``ok``
(run), or ``no: ...`` (does not compile or build, a failing exit, the
wrong output); `` (warns)`` marks a probe that built with a diagnostic.
Below the table, each compiler diagnostic is one line: the error or
warning that explains the cell, paths cut to file names.  Work files go under
``build/dktools/c23probe/<compiler>``.  Exit 0 whatever the answers;
the table is the result.
"""

from __future__ import annotations

import re
import shutil
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from . import procs
from .gitutil import ToolError
from .toolchain import Compiler

#: Flags every probe compiles with: datakit's language mode.
BASE_FLAGS = ("-std=gnu23", "-O2")

#: Seconds one compile or run may take.
STEP_TIMEOUT = 60.0

CALLS = "calls"
COMPILE = "compile"
RUN = "run"


@dataclass(frozen=True)
class Probe:
    """One question: a program and how to read the compiler's answer."""

    name: str
    kind: str
    source: str
    #: what a ``run`` probe must print
    expect: str = ""
    #: files written next to the source (name -> bytes), for #embed
    files: tuple[tuple[str, bytes], ...] = ()


_CONST_CALLER = "int g(int x) { return f(x) + f(x); }\n"
_PURE_CALLER = "int g(const int *p) { return f(p) + f(p); }\n"

PROBES: tuple[Probe, ...] = (
    Probe("gnu::const", CALLS, "[[gnu::const]] int f(int);\n" + _CONST_CALLER),
    Probe("[[unsequenced]]", CALLS, "int f(int) [[unsequenced]];\n" + _CONST_CALLER),
    Probe("gnu::pure", CALLS, "[[gnu::pure]] int f(const int *);\n" + _PURE_CALLER),
    Probe(
        "[[reproducible]]",
        CALLS,
        "int f(const int *) [[reproducible]];\n" + _PURE_CALLER,
    ),
    Probe("nullptr", COMPILE, "void *p = nullptr;\n"),
    Probe(
        "_BitInt(48)",
        COMPILE,
        "unsigned _BitInt(48) add(unsigned _BitInt(48) a, unsigned _BitInt(48) b) {\n"
        "    return a + b;\n}\n",
    ),
    Probe(
        "<stdcountof.h>",
        COMPILE,
        "#include <stdcountof.h>\nint a[3];\nunsigned long n = countof(a);\n",
    ),
    Probe(
        "#embed",
        RUN,
        "#include <stdio.h>\n"
        'static const unsigned char d[] = {\n#embed "probe.bin"\n};\n'
        'int main(void) { printf("%zu %d", sizeof d, d[3]); return 0; }\n',
        expect="4 7",
        files=(("probe.bin", bytes([4, 5, 6, 7])),),
    ),
    Probe(
        "<stdbit.h> functions",
        RUN,
        "#include <stdbit.h>\n#include <stdio.h>\n"
        "int main(void) {\n"
        '    printf("%u %u", stdc_leading_zeros(1u), stdc_bit_width(5u));\n'
        "    return 0;\n}\n",
        expect="31 3",
    ),
    Probe(
        "memalignment",
        RUN,
        "#include <stdio.h>\n#include <stdlib.h>\n"
        "int main(void) {\n"
        "    alignas(64) static char b[64];\n"
        '    printf("%d", memalignment(b) >= 64);\n'
        "    return 0;\n}\n",
        expect="1",
    ),
    Probe(
        "printf %w64d",
        RUN,
        "#include <stdint.h>\n#include <stdio.h>\n"
        "int main(void) {\n"
        '    printf("%w64d", (int64_t)-5);\n'
        "    return 0;\n}\n",
        expect="-5",
    ),
)

#: A direct call to f in assembly: aarch64 ``bl _f`` / ``b _f``, x86
#: ``call f`` / ``jmp f`` (Mach-O names carry a leading underscore).
_CALL = re.compile(r"^\s*(?:bl|b|call|callq|jmp)\s+_?f\b", re.MULTILINE)


def count_calls(asm: str) -> int:
    """How many direct calls to f the assembly makes."""
    return len(_CALL.findall(asm))


_PATH = re.compile(r"\S*/([^/\s:]+)(?=:)")
_SEVERE = re.compile(r"\b(?:fatal error|error|warning):")


def diagnostic(text: str) -> str:
    """The line of compiler output that says what went wrong: the first
    error or warning line (else the first non-blank one), with paths cut
    to file names ("" if the output is empty)."""
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    if not lines:
        return ""
    chosen = next((line for line in lines if _SEVERE.search(line)), lines[0])
    return _PATH.sub(r"\1", chosen)


def _warns(stderr: str) -> str:
    return " (warns)" if stderr.strip() else ""


def calls_verdict(returncode: int, asm: str, stderr: str) -> str:
    """The cell of a ``calls`` probe."""
    if returncode != 0:
        return "no: does not compile"
    calls = count_calls(asm)
    if calls == 1:
        return "folds" + _warns(stderr)
    if calls == 2:
        return "no fold" + _warns(stderr)
    return f"no: {calls} calls to f" + _warns(stderr)


def compile_verdict(returncode: int, stderr: str) -> str:
    """The cell of a ``compile`` probe."""
    if returncode != 0:
        return "no: does not compile"
    return "yes" + _warns(stderr)


def run_verdict(expect: str, output: str, build_stderr: str) -> str:
    """The cell of a ``run`` probe that built and ran."""
    if output == expect:
        return "ok" + _warns(build_stderr)
    return f"no: printed {output!r}, want {expect!r}"


def _text(data: bytes) -> str:
    return data.decode("utf-8", "replace")


def run_probe(compiler: Compiler, probe: Probe, work: Path) -> tuple[str, str]:
    """Compile (and for ``run``, run) one probe; return its cell and the
    compiler's diagnostic ("" when it printed none)."""
    work.mkdir(parents=True, exist_ok=True)
    stem = re.sub(r"[^A-Za-z0-9]+", "_", probe.name).strip("_")
    source = work / f"{stem}.c"
    source.write_text(probe.source)
    for name, data in probe.files:
        (work / name).write_bytes(data)
    cc = [compiler.cc, *BASE_FLAGS]
    if probe.kind == CALLS:
        done = procs.run(
            [*cc, "-S", "-o", "-", str(source)], cwd=work, timeout=STEP_TIMEOUT
        )
        err = _text(done.stderr)
        return calls_verdict(done.returncode, _text(done.stdout), err), diagnostic(err)
    if probe.kind == COMPILE:
        done = procs.run(
            [*cc, "-c", "-o", str(work / f"{stem}.o"), str(source)],
            cwd=work,
            timeout=STEP_TIMEOUT,
        )
        err = _text(done.stderr)
        return compile_verdict(done.returncode, err), diagnostic(err)
    if probe.kind == RUN:
        program = work / stem
        built = procs.run(
            [*cc, "-o", str(program), str(source)], cwd=work, timeout=STEP_TIMEOUT
        )
        err = _text(built.stderr)
        if built.returncode != 0:
            return "no: does not build", diagnostic(err)
        ran = procs.run([str(program)], cwd=work, timeout=STEP_TIMEOUT)
        if ran.returncode != 0:
            return f"no: exit {ran.returncode}", diagnostic(err)
        return run_verdict(probe.expect, _text(ran.stdout), err), diagnostic(err)
    raise ToolError(f"probe {probe.name}: unknown kind {probe.kind!r}")


def render_notes(notes: Sequence[tuple[str, str, str]]) -> str:
    """One line per (probe, compiler, diagnostic) that has a diagnostic."""
    lines = [f"  {probe} / {name}: {text}" for probe, name, text in notes if text]
    return ("diagnostics:\n" + "\n".join(lines) + "\n") if lines else ""


def render(names: Sequence[str], rows: Sequence[tuple[str, Sequence[str]]]) -> str:
    """The result table: a probe column, then one column per compiler."""
    header = ("probe", *names)
    table = [header, *((probe, *cells) for probe, cells in rows)]
    widths = [max(len(row[i]) for row in table) for i in range(len(header))]
    lines = []
    for row in table:
        cells = [cell.ljust(width) for cell, width in zip(row, widths, strict=True)]
        lines.append("  ".join(cells).rstrip())
    return "\n".join(lines) + "\n"


def run(repo: Path, compilers: Sequence[Compiler]) -> int:
    """Run every probe with every compiler and print the table."""
    root = repo / "build" / "dktools" / "c23probe"
    for compiler in compilers:
        compiler.check()
        shutil.rmtree(root / compiler.name, ignore_errors=True)
    rows = []
    notes = []
    for probe in PROBES:
        cells = []
        for compiler in compilers:
            cell, note = run_probe(compiler, probe, root / compiler.name)
            cells.append(cell)
            notes.append((probe.name, compiler.name, note))
        rows.append((probe.name, cells))
    print(render([c.name for c in compilers], rows), end="")
    print(render_notes(notes), end="")
    return 0
