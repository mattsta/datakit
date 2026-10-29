"""Library output audit for datakit.

datakit runs inside other applications, so library code must not write to
the process's standard streams on its own: a caller embedding datakit owns
stdout and stderr.  This audit finds every call that writes to them in
``src/`` (vendored ``memshard`` excepted), outside in-file test code
(``#ifdef DATAKIT_TEST``), and sorts each one:

  requested    inside a function whose job is output the caller asked for
               (its name has Repr, Print, Dump, Say or Show in it: flexRepr,
               multidictPrintStats, databoxReprSay, ...)
  unrequested  everywhere else: diagnostics, warnings and errors printed
               from API code; these are what the error-reporting design
               replaces
  debug        compiled only under a debug or tracing switch (an ``#if`` /
               ``#ifdef`` of a name holding DEBUG, VERBOSE, NOISY, TRACE or
               DBG) or only with assertions on (``#ifndef NDEBUG``)
  dead         inside ``#if 0``
  tool         in an operator tool whose output is its job (dataspeed's
               benchmark reports, memtest's interactive run): the caller
               runs it to read what it prints
  test         in a test-only file (``*Test.c``, ``*Bench.c``,
               datakit-test.c, perfRegression.c, ctest.h, the fuzz
               targets in src/fuzz), outside
               DATAKIT_TEST guards but never part of an application's use
               of the API

A call that writes to a stream or descriptor the caller passed in
(``fprintf(fp, ...)``, ``fwrite(buf, 1, n, out)``) is the caller's output
and is not listed; one whose target is ``stdout``, ``stderr``, descriptor
1 or 2 (or ``STDOUT_FILENO`` / ``STDERR_FILENO``) is.

Discovery is lexical (atomicaudit's lexer: comments and string literals
are opaque), so a call inside a macro definition is listed at the
definition, under ``#define NAME``.

``check`` exits nonzero while any unrequested output remains.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

from .atomicaudit import EXCLUDE_DIRS, Token, lex, source_files
from .coverage import test_lines

#: Calls that always write to stdout or stderr.
STDIO_CALLS = frozenset({"printf", "vprintf", "puts", "putchar", "perror"})

#: Calls whose target is an argument: name -> index of the target.
STREAM_CALLS = {
    "fprintf": 0,
    "vfprintf": 0,
    "fputs": 1,
    "fputc": 1,
    "putc": 1,
    "fwrite": 3,
    "fflush": 0,
    "dprintf": 0,
    "vdprintf": 0,
    "write": 0,
}

#: Targets that are the process's own standard streams.
STD_TARGETS = frozenset(
    {"stdout", "stderr", "1", "2", "STDOUT_FILENO", "STDERR_FILENO"}
)

#: Function (or macro) names whose job is output the caller asked for.
REQUESTED_RE = re.compile(r"Repr|Print|Dump|Say|Show|REPR|PRINT|DUMP")

#: Test-only files compiled outside DATAKIT_TEST guards.
TEST_FILE_RE = re.compile(
    r"(?:Test|Bench)\.c$|(?:^|/)(?:datakit-test|perfRegression)\.c$|(?:^|/)ctest\.h$"
    r"|(?:^|/)src/fuzz/[^/]+\.[ch]$"
)

#: Operator tools: library modules whose output is what the caller runs them
#: for.
TOOL_FILE_RE = re.compile(r"(?:^|/)(?:dataspeed|memtest)\.c$")

#: A conditional's switch that turns on debugging output.
DEBUG_SWITCH_RE = re.compile(r"DEBUG|VERBOSE|NOISY|TRACE|DBG|DUMP")

#: The categories, in report order.
CATEGORIES = ("unrequested", "debug", "dead", "requested", "tool", "test")


@dataclass(frozen=True, slots=True)
class Site:
    path: str
    line: int
    call: str
    target: str  # "stdout"/"stderr"/... or the call itself for STDIO_CALLS
    context: str  # enclosing function, or "#define NAME"
    category: str  # requested / unrequested / test


@dataclass
class AuditResult:
    sites: list[Site] = field(default_factory=list)

    def by_category(self, category: str) -> list[Site]:
        return [s for s in self.sites if s.category == category]

    @property
    def clean(self) -> bool:
        return not self.by_category("unrequested")


def _define_lines(src: str) -> dict[int, str]:
    """Line number -> macro name, for every line of a #define (its
    continuation lines included)."""
    out: dict[int, str] = {}
    lines = src.splitlines()
    i = 0
    while i < len(lines):
        m = re.match(r"\s*#\s*define\s+([A-Za-z_][A-Za-z0-9_]*)", lines[i])
        if not m:
            i += 1
            continue
        name = m.group(1)
        j = i
        out[j + 1] = name
        while lines[j].rstrip().endswith("\\") and j + 1 < len(lines):
            j += 1
            out[j + 1] = name
        i = j + 1
    return out


def _first_arg(tokens: list[Token], open_idx: int, index: int) -> str:
    """The text of argument 'index' of the call whose '(' is at open_idx,
    when it is a single token; '' otherwise."""
    depth = 0
    arg = 0
    collected: list[str] = []
    for tok in tokens[open_idx + 1 :]:
        t = tok.text
        if t in "([{":
            depth += 1
        elif t in ")]}":
            if depth == 0:
                break
            depth -= 1
        elif t == "," and depth == 0:
            arg += 1
            continue
        if arg == index:
            collected.append(t)
        elif arg > index:
            break
    return collected[0] if len(collected) == 1 else ""


_IF_RE = re.compile(r"\s*#\s*(ifdef|ifndef|if)\b\s*(.*)")
_ELSE_RE = re.compile(r"\s*#\s*(else|elif)\b")
_ENDIF_RE = re.compile(r"\s*#\s*endif\b")


def _conditionals(src: str) -> tuple[set[int], set[int]]:
    """(dead lines, debug lines): lines inside the true branch of an
    ``#if 0``, and lines compiled only under a debug switch or with
    assertions on.  An #else / #elif branch of either is neither."""
    dead: set[int] = set()
    debug: set[int] = set()
    stack: list[str] = []  # per open conditional: "dead", "debug" or ""
    for number, line in enumerate(src.splitlines(), 1):
        m = _IF_RE.match(line)
        if m:
            kind, cond = m.group(1), m.group(2).split("/*")[0].strip()
            if kind == "if" and cond == "0":
                stack.append("dead")
            elif (
                kind == "ifndef"
                and cond == "NDEBUG"
                or kind != "ifndef"
                and DEBUG_SWITCH_RE.search(cond)
                and "!" not in cond
            ):
                stack.append("debug")
            else:
                stack.append("")
        elif _ELSE_RE.match(line) and stack:
            stack[-1] = ""
        elif _ENDIF_RE.match(line) and stack:
            stack.pop()
        if "dead" in stack:
            dead.add(number)
        elif "debug" in stack:
            debug.add(number)
    return dead, debug


def _preprocessor_lines(src: str) -> set[int]:
    """Lines of preprocessor directives other than #define (they hold no
    calls but can hold names that look like them)."""
    out: set[int] = set()
    for number, line in enumerate(src.splitlines(), 1):
        if re.match(r"\s*#\s*(?!define\b)\w", line):
            out.add(number)
    return out


def scan_text(path: str, src: str) -> list[Site]:
    tokens, _ = lex(src)
    skip = test_lines(src) | _preprocessor_lines(src)
    defines = _define_lines(src)
    dead, debug = _conditionals(src)
    test_file = bool(TEST_FILE_RE.search(path))
    tool_file = bool(TOOL_FILE_RE.search(path))
    sites: list[Site] = []

    depth = 0
    function = ""  # name of the function whose body we are in
    candidate = ""  # last top-level identifier followed by '('
    for i, tok in enumerate(tokens):
        t = tok.text
        if tok.line in defines:
            # macro bodies have their own braces; they do not open functions
            if t in STDIO_CALLS or t in STREAM_CALLS:
                pass
            else:
                continue
        elif t == "{":
            if depth == 0:
                function = candidate
            depth += 1
            continue
        elif t == "}":
            depth = max(depth - 1, 0)
            if depth == 0:
                function = ""
            continue
        nxt = tokens[i + 1].text if i + 1 < len(tokens) else ""
        if depth == 0 and nxt == "(" and re.match(r"[A-Za-z_]\w*$", t):
            candidate = t
        if nxt != "(" or tok.line in skip:
            continue
        prev = tokens[i - 1].text if i > 0 else ""
        arrow = prev == ">" and i > 1 and tokens[i - 2].text == "-"
        if prev == "." or arrow:
            continue  # a struct member that happens to share the name
        if t in STDIO_CALLS:
            target = "stdout" if t != "perror" else "stderr"
        elif t in STREAM_CALLS:
            target = _first_arg(tokens, i + 1, STREAM_CALLS[t])
            if target not in STD_TARGETS:
                continue  # the caller's stream
            if t == "fflush":
                continue  # flushing writes nothing of its own
        else:
            continue
        context = f"#define {defines[tok.line]}" if tok.line in defines else function
        if test_file:
            category = "test"
        elif tool_file:
            category = "tool"
        elif tok.line in dead:
            category = "dead"
        elif tok.line in debug:
            category = "debug"
        elif context and REQUESTED_RE.search(context.removeprefix("#define ")):
            category = "requested"
        else:
            category = "unrequested"
        sites.append(
            Site(path, tok.line, t, target, context or "(file scope)", category)
        )
    return sites


def run_audit(repo: Path, paths: list[Path] | None = None) -> AuditResult:
    result = AuditResult()
    files = paths if paths is not None else source_files(repo)
    for p in files:
        rel = str(p.relative_to(repo)) if p.is_absolute() else str(p)
        result.sites += scan_text(rel, (repo / rel).read_text(errors="replace"))
    return result


def summary_line(result: AuditResult) -> str:
    counts = {c: len(result.by_category(c)) for c in CATEGORIES}
    files = len({s.path for s in result.by_category("unrequested")})
    return (
        f"outputaudit: {counts['unrequested']} unrequested output calls in {files} files; "
        f"{counts['debug']} debug-only, {counts['dead']} in #if 0, "
        f"{counts['requested']} requested (repr/print), {counts['tool']} in operator "
        f"tools, {counts['test']} in test-only files"
    )


def render_report(result: AuditResult) -> str:
    md = ["# Library output audit", "", summary_line(result), ""]
    md += [
        "Every call in library code that writes to stdout or stderr, outside",
        "in-file test code.  `unrequested` calls print without the caller asking;",
        "`debug` ones only under a debug switch or with assertions on; `dead` ones",
        "are in `#if 0`; `requested` ones are inside Repr/Print/Dump functions;",
        "`tool` ones are in operator tools (dataspeed, memtest).",
        "",
    ]
    for category in CATEGORIES:
        sites = result.by_category(category)
        md += [f"## {category} ({len(sites)})", ""]
        if not sites:
            md += ["none", ""]
            continue
        per_file: dict[str, list[Site]] = {}
        for s in sites:
            per_file.setdefault(s.path, []).append(s)
        md += ["| file | calls | functions (line: call) |", "| --- | --- | --- |"]
        for path in sorted(per_file, key=lambda p: (-len(per_file[p]), p)):
            group = per_file[path]
            funcs: dict[str, list[str]] = {}
            for s in group:
                funcs.setdefault(s.context, []).append(f"{s.line}: {s.call}")
            detail = "; ".join(
                f"`{name}` ({', '.join(calls)})" for name, calls in funcs.items()
            )
            md.append(f"| {path} | {len(group)} | {detail} |")
        md.append("")
    return "\n".join(md) + "\n"


__all__ = [
    "EXCLUDE_DIRS",
    "AuditResult",
    "Site",
    "render_report",
    "run_audit",
    "scan_text",
    "summary_line",
]
