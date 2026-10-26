"""``dktools defconst``: ``#define NAME number`` becomes static constexpr.

C23's constexpr names a typed constant; a numeric #define is an untyped
textual substitution. A numeric #define (an integer literal, optionally
parenthesized, then an optional comment) becomes
``static constexpr T NAME = LITERAL;`` with T the literal's own type
(plain decimal: int, or int64_t when too big; hex: int, unsigned when it
needs the top bit, int64_t/uint64_t when wider; u: unsigned; L/LL:
int64_t; UL/ULL: uint64_t), so arithmetic does not change.

A name stays a macro (the report says why) when:

- the preprocessor reads it: it appears in an #if, #elif, #ifdef,
  #ifndef, #undef or #pragma line anywhere, or beside # / ## in a macro
  body (an override, a feature test, a template parameter);
- it is defined more than once in the tree;
- it is defined in a public header (any src/**/*.h except *Internal*.h
  and *Impl*.h), where a consumer may test it with #if;
- its #define sits inside a function body (a macro ignores scope, a
  constexpr local would not).

``--report`` (default with ``--dry-run``) lists every numeric #define
with its verdict; without it the convertible ones in the given files are
rewritten. Exits 0, or 2 for a missing file.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from .clex import code_mask
from .gitutil import ToolError

CONVERT = "convert"
PREPROCESSOR = "the preprocessor reads it"
MULTIPLE = "defined more than once"
PUBLIC = "public header"
IN_BLOCK = "defined inside a function"
UNUSED = "nothing uses it (a person decides: remove it?)"

NUMERIC_DEFINE = re.compile(
    rb"(?m)^(?P<indent>[ \t]*)#[ \t]*define[ \t]+(?P<name>[A-Za-z_]\w*)[ \t]+"
    rb"(?P<literal>\(?-?[0-9][0-9A-Fa-fxX']*[uUlL]*\)?)"
    rb"(?P<rest>[ \t]*(?:/\*.*?\*/|//.*)?)[ \t]*$"
)
DIRECTIVE_LINE = re.compile(rb"(?m)^[ \t]*#[ \t]*(\w+)((?:[^\n]|\\\n)*)")
READING_DIRECTIVES = frozenset(
    (b"if", b"elif", b"ifdef", b"ifndef", b"elifdef", b"elifndef", b"undef", b"pragma")
)
INTERNAL_HEADER = re.compile(r"(Internal|Impl)[^/]*\.h$")


DEFINE = "define"
CONSTEXPR = "constexpr"
#: the verdict under which a constexpr constant is wrong (an #if reads
#: the name and sees 0): it goes back to #define. A name defined more than
#: once is fine as constexpr (one constant per function or file).
MUST_BE_MACRO = frozenset((PREPROCESSOR,))

CONSTEXPR_LINE = re.compile(
    rb"(?m)^(?P<indent>[ \t]*)static[ \t]+constexpr[ \t]+(?:int|unsigned|int64_t|uint64_t)"
    rb"[ \t]+(?P<name>[A-Za-z_]\w*)[ \t]*=[ \t]*"
    rb"(?P<literal>\(?-?[0-9][0-9A-Fa-fxX']*[uUlL]*\)?)[ \t]*;"
    rb"(?P<rest>[ \t]*(?:/\*.*?\*/|//.*)?)[ \t]*$"
)


@dataclass(frozen=True)
class Constant:
    path: str
    line: int
    name: str
    literal: str
    verdict: str
    form: str = DEFINE  # how it is spelled now


def literal_type(literal: str) -> str | None:
    """The C type of an integer literal (LP64), as datakit spells it; None
    for anything that is not one."""
    text = literal.strip()
    if text.startswith("(") and text.endswith(")"):
        text = text[1:-1].strip()
    negative = text.startswith("-")
    text = text.lstrip("-").replace("'", "")
    m = re.fullmatch(r"(0[xX][0-9A-Fa-f]+|[0-9]+)([uU]?)([lL]{0,2})([uU]?)", text)
    if not m:
        return None
    digits, u1, longs, u2 = m.groups()
    unsigned = bool(u1 or u2)
    hexa = digits.lower().startswith("0x")
    value = int(digits, 16 if hexa else 10)
    if negative:
        value = -value
    if longs:
        return "uint64_t" if unsigned else "int64_t"
    if unsigned:
        return "unsigned" if value <= 0xFFFFFFFF else "uint64_t"
    if -(2**31) <= value < 2**31:
        return "int"
    if hexa and value <= 0xFFFFFFFF:
        return "unsigned"
    if hexa and value >= 2**63:
        return "uint64_t"
    return "int64_t"


def _depth_at(src: bytes, mask: bytearray, at: int) -> int:
    depth = 0
    for i in range(at):
        if mask[i]:
            if src[i] == 0x7B:
                depth += 1
            elif src[i] == 0x7D:
                depth -= 1
    return depth


def _preprocessor_names(texts: list[bytes]) -> set[bytes]:
    """Names any #if / #ifdef / #undef / #pragma line reads, and names
    beside # or ## in a macro body."""
    names: set[bytes] = set()
    bodies: dict[bytes, set[bytes]] = {}  # macro -> the names its body uses
    for src in texts:
        for d in DIRECTIVE_LINE.finditer(src):
            keyword, body = d.group(1), d.group(2)
            if keyword in READING_DIRECTIVES:
                names.update(re.findall(rb"[A-Za-z_]\w*", body))
            elif keyword == b"define":
                names.update(re.findall(rb"##\s*([A-Za-z_]\w*)", body))
                names.update(re.findall(rb"([A-Za-z_]\w*)\s*##", body))
                names.update(re.findall(rb"(?<![#\w])#\s*([A-Za-z_]\w*)", body))
                words = re.findall(rb"[A-Za-z_]\w*", body)
                if words:
                    bodies.setdefault(words[0], set()).update(words[1:])
    # a name a read macro expands to is read too (#if PACK_STORAGE_BITS
    # with #define PACK_STORAGE_BITS DIRECT_STORAGE_BITS)
    todo = list(names)
    while todo:
        for used in bodies.get(todo.pop(), ()):
            if used not in names:
                names.add(used)
                todo.append(used)
    return names


def tree_files(repo: Path) -> list[str]:
    """src/**/*.c and *.h, the vendored src/memshard excluded."""
    src = repo / "src"
    return sorted(
        str(p.relative_to(repo))
        for p in src.rglob("*")
        if p.suffix in (".c", ".h") and "memshard" not in p.relative_to(src).parts
    )


def _vendored_texts(repo: Path) -> list[bytes]:
    """deps/**/*.c and *.h: headers datakit includes read its #defines
    too (varintPacked.h's #if PACK_STORAGE_BITS)."""
    deps = repo / "deps"
    if not deps.is_dir():
        return []
    return [p.read_bytes() for p in sorted(deps.rglob("*")) if p.suffix in (".c", ".h")]


def classify(repo: Path, paths: list[str]) -> list[Constant]:
    """Every numeric #define and every static constexpr integer constant
    in 'paths' with its verdict (the whole tree and the vendored headers
    decide the preprocessor, defined-twice and unused checks)."""
    tree = {p: (repo / p).read_bytes() for p in tree_files(repo)}
    for p in paths:
        tree.setdefault(p, (repo / p).read_bytes())
    everything = list(tree.values()) + _vendored_texts(repo)
    read_by_preprocessor = _preprocessor_names(everything)
    defined: dict[bytes, int] = {}
    for src in tree.values():
        for d in DIRECTIVE_LINE.finditer(src):
            if d.group(1) == b"define":
                m = re.match(rb"\s*([A-Za-z_]\w*)", d.group(2))
                if m:
                    defined[m.group(1)] = defined.get(m.group(1), 0) + 1
        for c in CONSTEXPR_LINE.finditer(src):
            defined[c.group("name")] = defined.get(c.group("name"), 0) + 1
    out = []
    for path in paths:
        src = tree[path]
        mask = None
        found = [(m, DEFINE) for m in NUMERIC_DEFINE.finditer(src)]
        found += [(m, CONSTEXPR) for m in CONSTEXPR_LINE.finditer(src)]
        for m, form in sorted(found, key=lambda f: f[0].start()):
            literal = m.group("literal").decode()
            if literal_type(literal) is None:
                continue
            name = m.group("name")
            word = re.compile(rb"\b" + re.escape(name) + rb"\b")
            uses = sum(len(word.findall(t)) for t in everything) - 1
            if name in read_by_preprocessor:
                verdict = PREPROCESSOR
            elif defined.get(name, 0) > 1:
                verdict = MULTIPLE
            elif path.endswith(".h") and not INTERNAL_HEADER.search(path):
                verdict = PUBLIC
            elif uses <= 0:
                verdict = UNUSED
            else:
                mask = mask if mask is not None else code_mask(src)
                verdict = IN_BLOCK if _depth_at(src, mask, m.start()) else CONVERT
            line = src.count(b"\n", 0, m.start()) + 1
            out.append(Constant(path, line, name.decode(), literal, verdict, form))
    return out


def rewrite(src: bytes, names: set[bytes]) -> bytes:
    """'src' with each numeric #define of a name in 'names' made a static
    constexpr of the literal's type (a trailing comment kept)."""

    def replace(m: re.Match[bytes]) -> bytes:
        name = m.group("name")
        literal = m.group("literal")
        ctype = literal_type(literal.decode())
        if name not in names or ctype is None:
            return m.group(0)
        rest = m.group("rest").strip()
        tail = b" " + rest if rest else b""
        return (
            m.group("indent")
            + b"static constexpr "
            + ctype.encode()
            + b" "
            + name
            + b" = "
            + literal
            + b";"
            + tail
        )

    return NUMERIC_DEFINE.sub(replace, src)


def restore(src: bytes, names: set[bytes]) -> bytes:
    """'src' with each static constexpr constant of a name in 'names' made
    a #define again (the rules say it must be a macro)."""

    def replace(m: re.Match[bytes]) -> bytes:
        if m.group("name") not in names:
            return m.group(0)
        rest = m.group("rest").strip()
        tail = b" " + rest if rest else b""
        return (
            m.group("indent")
            + b"#define "
            + m.group("name")
            + b" "
            + m.group("literal")
            + tail
        )

    return CONSTEXPR_LINE.sub(replace, src)


def run(repo: Path, paths: tuple[str, ...], dry_run: bool) -> int:
    """Report every numeric constant; convert #defines the rules allow and
    turn back constexpr constants the rules say must be macros, so a rerun
    with better rules converges."""
    targets = list(paths) or tree_files(repo)
    for path in targets:
        if not (repo / path).is_file():
            raise ToolError(f"{path}: no such file")
    constants = classify(repo, targets)
    tally: dict[str, int] = {}
    convert: dict[str, set[bytes]] = {}
    back: dict[str, set[bytes]] = {}
    for c in constants:
        if c.form == DEFINE and c.verdict == CONVERT:
            convert.setdefault(c.path, set()).add(c.name.encode())
        elif c.form == CONSTEXPR and c.verdict in MUST_BE_MACRO:
            back.setdefault(c.path, set()).add(c.name.encode())
            print(f"  {c.path}:{c.line}: {c.name}: back to #define ({c.verdict})")
        if c.form == DEFINE:
            tally[c.verdict] = tally.get(c.verdict, 0) + 1
            if c.verdict != CONVERT:
                print(f"  {c.path}:{c.line}: {c.name}: {c.verdict}")
        elif c.verdict == UNUSED:
            print(f"  {c.path}:{c.line}: {c.name}: {c.verdict}")
    for path in sorted(set(convert) | set(back)):
        print(
            f"  {path}: {len(convert.get(path, ()))} to constexpr, "
            f"{len(back.get(path, ()))} back to #define"
        )
        if not dry_run:
            target = repo / path
            src = rewrite(target.read_bytes(), convert.get(path, set()))
            target.write_bytes(restore(src, back.get(path, set())))
    summary = ", ".join(f"{n} {v}" for v, n in sorted(tally.items()))
    verb = "would convert" if dry_run else "converted"
    undone = sum(len(v) for v in back.values())
    print(
        f"defconst: {verb} {tally.get(CONVERT, 0)}, back to #define {undone}; "
        f"#defines: {summary or 'none'}"
    )
    return 0
