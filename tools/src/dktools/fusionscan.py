"""Read-then-write pairs in datakit: two searches where one could do.

A change that first asks a container about a key (``multimapExists(m,
key)``) and then changes it by the same key (``multimapInsert(&m, row)``)
searches the container twice.  ``fusionscan`` lists every such pair inside
one function of ``src/`` library code (test-only files as outputaudit
names them, in-file test code, ``#if 0`` and macro bodies excepted) for
review: some are needed (the read decides
something the write cannot), the rest are candidates for a one-search form.

A pair is a *read* call followed, in the same function, by a *write* call
of the same container family:

  family   the longest header name under ``src/`` that begins the call's
           name (``multiOrderedSetExists`` is ``multiOrderedSet``, not
           ``multi`` + ``Ordered`` + ``Set``); families match when one
           begins the other (``multimap`` and ``multimapFull``)
  read     the rest of the name starts with Find, Exists, Lookup, Get,
           Contains, Has, Search or Peek
  write    it starts with Insert, Add, Delete, Remove, Replace, Set, Incr,
           Update, Push, Append, Resize, Del or Put

and the two calls name the same container (first argument) and the same
key (second argument), compared by root identifier: ``&m->m`` and ``m`` are
both ``m``, ``elements[0]`` and ``elements`` both ``elements``, and a row
array built as ``elements[] = {&key, ...}`` in the function names
``key``.  A read
whose result (the variable it is assigned to) a write takes is where that
write acts (already one search), and is not paired.

Discovery is lexical (atomicaudit's lexer), so calls through function
pointers, or made by macros, are not seen.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from .atomicaudit import Token, lex, source_files
from .coverage import test_lines
from .outputaudit import (
    TEST_FILE_RE,
    _conditionals,
    _define_lines,
    _preprocessor_lines,
)

READ_VERBS = ("Find", "Exists", "Lookup", "Get", "Contains", "Has", "Search", "Peek")
WRITE_VERBS = (
    "Insert",
    "Add",
    "Delete",
    "Remove",
    "Replace",
    "Set",
    "Incr",
    "Update",
    "Push",
    "Append",
    "Resize",
    "Del",
    "Put",
)

#: Names that look like calls but are not.
NOT_CALLS = frozenset(
    {"if", "for", "while", "switch", "return", "sizeof", "_Alignof", "defined"}
)

#: C words skipped when looking for an argument's root identifier.
TYPE_WORDS = frozenset(
    {"const", "void", "char", "int", "unsigned", "signed", "struct", "volatile"}
)

_IDENT = re.compile(r"[A-Za-z_]\w*$")


@dataclass(frozen=True, slots=True)
class Call:
    line: int
    name: str
    family: str
    verb: str
    args: tuple[tuple[str, ...], ...]  # token texts per argument
    result: str  # the variable the call's value is assigned to, or ""


@dataclass(frozen=True, slots=True)
class Pair:
    path: str
    function: str
    read: Call
    write: Call
    container: str
    key: str


def families(repo: Path) -> list[str]:
    """Container families: header names under src/, longest first."""
    names = {p.stem for p in source_files(repo) if p.suffix == ".h"}
    return sorted(names, key=len, reverse=True)


def classify(name: str, known: list[str]) -> tuple[str, str]:
    """(family, verb) for a call name: verb is "" when the name is not a
    read or write of a known family."""
    for family in known:
        if name.startswith(family) and len(name) > len(family):
            rest = name[len(family) :]
            for verb in READ_VERBS + WRITE_VERBS:
                if rest.startswith(verb):
                    return family, verb
            return family, ""
    return "", ""


def root(arg: tuple[str, ...]) -> str:
    """The first identifier of an argument, past '&', '*', parentheses and
    cast words."""
    for t in arg:
        if _IDENT.match(t) and t not in TYPE_WORDS:
            return t
    return ""


def _arguments(tokens: list[Token], open_idx: int) -> tuple[tuple[str, ...], ...]:
    args: list[list[str]] = [[]]
    depth = 0
    for tok in tokens[open_idx + 1 :]:
        t = tok.text
        if t in "([{":
            depth += 1
        elif t in ")]}":
            if depth == 0:
                break
            depth -= 1
        elif t == "," and depth == 0:
            args.append([])
            continue
        args[-1].append(t)
    return tuple(tuple(a) for a in args if a)


def _row_alias(tokens: list[Token], i: int) -> tuple[str, str] | None:
    """(name, key) for ``name[...] = {&key, ...}`` starting at token i: a
    row array whose first element is the key it names."""
    if not _IDENT.match(tokens[i].text) or i + 1 >= len(tokens):
        return None
    j = i + 1
    if tokens[j].text != "[":
        return None
    while j < len(tokens) and tokens[j].text != "]":
        j += 1
    if j + 2 >= len(tokens) or tokens[j + 1].text != "=" or tokens[j + 2].text != "{":
        return None
    first: list[str] = []
    for tok in tokens[j + 3 :]:
        if tok.text in (",", "}"):
            break
        first.append(tok.text)
    key = root(tuple(first))
    return (tokens[i].text, key) if key else None


def _assigned_to(tokens: list[Token], name_idx: int) -> str:
    """The variable in ``x = name(...)`` or ``T *x = name(...)``, else ""."""
    if name_idx >= 2 and tokens[name_idx - 1].text == "=":
        target = tokens[name_idx - 2].text
        if _IDENT.match(target):
            return target
    return ""


def scan_text(path: str, src: str, known: list[str]) -> list[Pair]:
    tokens, _ = lex(src)
    defines = _define_lines(src)
    dead, _debug = _conditionals(src)
    skip = test_lines(src) | _preprocessor_lines(src) | dead | set(defines)

    pairs: list[Pair] = []
    depth = 0
    function = ""
    candidate = ""
    calls: list[Call] = []
    aliases: dict[str, str] = {}  # row array -> the key it begins with

    def key_of(arg: tuple[str, ...]) -> str:
        name = root(arg)
        return aliases.get(name, name)

    def close_function() -> None:
        reads = [c for c in calls if c.verb in READ_VERBS]
        writes = [c for c in calls if c.verb in WRITE_VERBS]
        for r in reads:
            if len(r.args) < 2:
                continue
            container, key = root(r.args[0]), key_of(r.args[1])
            if not container or not key:
                continue
            if r.result and any(r.result in a for w in writes for a in w.args):
                continue  # a write acts where this read landed
            for w in writes:
                if w.line < r.line or len(w.args) < 2:
                    continue
                if not (w.family.startswith(r.family) or r.family.startswith(w.family)):
                    continue
                if root(w.args[0]) != container or key_of(w.args[1]) != key:
                    continue
                pairs.append(Pair(path, function, r, w, container, key))
                break

    for i, tok in enumerate(tokens):
        t = tok.text
        if tok.line in defines:
            continue
        if t == "{":
            if depth == 0:
                function = candidate
                calls = []
                aliases.clear()
            depth += 1
            continue
        if depth > 0 and tok.line not in skip:
            alias = _row_alias(tokens, i)
            if alias:
                aliases[alias[0]] = alias[1]
        if t == "}":
            depth = max(depth - 1, 0)
            if depth == 0 and function:
                close_function()
                function = ""
            continue
        nxt = tokens[i + 1].text if i + 1 < len(tokens) else ""
        if nxt != "(" or not _IDENT.match(t) or t in NOT_CALLS:
            continue
        if depth == 0:
            candidate = t
            continue
        if tok.line in skip:
            continue
        prev = tokens[i - 1].text if i > 0 else ""
        if prev == "." or (prev == ">" and i > 1 and tokens[i - 2].text == "-"):
            continue
        family, verb = classify(t, known)
        if not verb:
            continue
        calls.append(
            Call(
                tok.line,
                t,
                family,
                verb,
                _arguments(tokens, i + 1),
                _assigned_to(tokens, i),
            )
        )
    return pairs


def run_scan(repo: Path, paths: list[Path] | None = None) -> list[Pair]:
    known = families(repo)
    files = paths if paths is not None else source_files(repo)
    pairs: list[Pair] = []
    for p in files:
        rel = str(p.relative_to(repo)) if p.is_absolute() else str(p)
        if not rel.endswith(".c") or TEST_FILE_RE.search(rel):
            continue
        pairs += scan_text(rel, (repo / rel).read_text(errors="replace"), known)
    return pairs


def summary_line(pairs: list[Pair]) -> str:
    files = len({p.path for p in pairs})
    functions = len({(p.path, p.function) for p in pairs})
    return (
        f"fusionscan: {len(pairs)} read-then-write pairs in {functions} "
        f"functions across {files} files"
    )


def render_report(pairs: list[Pair]) -> str:
    md = ["# Read-then-write pairs", "", summary_line(pairs), ""]
    md += [
        "Each row: a read of a container by a key, then a write of the same",
        "container by the same key, in one function.  Review each: keep it when",
        "the read decides something the write cannot, or fuse it into one search.",
        "",
    ]
    if not pairs:
        return "\n".join(md + ["none", ""]) + "\n"
    md += [
        "| file | function | read | write | container / key |",
        "| --- | --- | --- | --- | --- |",
    ]
    for p in sorted(pairs, key=lambda x: (x.path, x.read.line)):
        md.append(
            f"| {p.path} | `{p.function}` | {p.read.line}: `{p.read.name}` | "
            f"{p.write.line}: `{p.write.name}` | `{p.container}` / `{p.key}` |"
        )
    return "\n".join(md) + "\n"


__all__ = [
    "Call",
    "Pair",
    "classify",
    "families",
    "render_report",
    "root",
    "run_scan",
    "scan_text",
    "summary_line",
]
