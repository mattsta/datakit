"""``dktools stdattr``: ``__attribute__((...))`` becomes the standard form.

C23 spells attributes ``[[...]]``; GNU ones are ``[[gnu::name]]``. Each
``__attribute__((a, b(x)))`` in code or a macro body becomes
``[[gnu::a, gnu::b(x)]]`` (``deprecated`` is standard, ``optnone`` is
``clang::optnone``; ``aligned(N)`` on an object is the standard
``alignas(N)``), placed where a standard attribute means the same thing:

- at the start of the declaration (before ``static``, ``inline``, the
  type): a ``[[...]]`` after a specifier would attach to the type, so an
  attribute written after specifiers or after the declarator moves there;
- right after ``struct``/``union``/``enum`` it stays (a type attribute);
- in a macro body it moves to the start of the body.

Then every use of an attribute macro (a macro whose body is only
attributes, such as ``DK_FN_CONST`` or ``cleaner(fn)``) that follows other
specifiers moves to the start of its declaration, with its arguments.

An attribute after a struct body (``} __attribute__((packed)) name;``)
is reported for a person. Comments and literals never change.
"""

from __future__ import annotations

import re
from pathlib import Path

from .clex import code_mask
from .gitutil import ToolError

AFTER_BODY = "attribute after a struct body: move it after 'struct' by hand"
NOT_A_DECLARATION = "the text before it is not one declaration's specifiers: by hand"

DIRECTIVE = re.compile(rb"(?m)^[ \t]*#(?:[^\n\\]|\\.)*", re.DOTALL)
DEFINE_HEAD = re.compile(rb"[ \t]*#[ \t]*define[ \t]+[A-Za-z_]\w*(?:\([^)]*\))?[ \t]*")
IDENT = re.compile(rb"[A-Za-z_]\w*")
TYPE_KEYWORDS = (b"struct", b"union", b"enum")
#: after '} __attribute__((...))': a typedef name and ';' (a struct body's
#: attribute), not the next function
TYPEDEF_NAME_NEXT = re.compile(rb"\s*(?:[A-Za-z_]\w*\s*)?;")
STANDARD = {
    b"deprecated": b"deprecated",
    b"noreturn": b"noreturn",
    b"unused": b"maybe_unused",
    b"warn_unused_result": b"nodiscard",
}
CLANG = {b"optnone"}


def _match_paren(src: bytes, open_at: int) -> int:
    """Index of the ')' closing the '(' at 'open_at' (strings skipped)."""
    depth = 0
    i = open_at
    while i < len(src):
        c = src[i]
        if c in b"\"'":
            j = i + 1
            while j < len(src) and src[j] != c:
                j += 2 if src[j] == 0x5C else 1
            i = j
        elif c == 0x28:
            depth += 1
        elif c == 0x29:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def _split_top(text: bytes) -> list[bytes]:
    items, depth, start, i = [], 0, 0, 0
    while i < len(text):
        c = text[i]
        if c in b"\"'":
            j = i + 1
            while j < len(text) and text[j] != c:
                j += 2 if text[j] == 0x5C else 1
            i = j
        elif c in b"([":
            depth += 1
        elif c in b")]":
            depth -= 1
        elif c == 0x2C and depth == 0:
            items.append(text[start:i])
            start = i + 1
        i += 1
    items.append(text[start:])
    return [re.sub(rb"\s+", b" ", it.strip()) for it in items if it.strip()]


def _standard(items: list[bytes], on_type: bool) -> bytes:
    """'[[...]]' (and 'alignas(N) ') for one attribute list."""
    attrs, aligns = [], []
    for item in items:
        m = IDENT.match(item)
        name = m.group() if m else item
        args = item[len(name) :].strip()
        if name == b"aligned" and not on_type:
            aligns.append(b"alignas" + args)
        elif name in STANDARD:
            attrs.append(STANDARD[name] + args)
        elif name in CLANG:
            attrs.append(b"clang::" + item)
        else:
            attrs.append(b"gnu::" + item)
    out = b" ".join(aligns)
    if attrs:
        out += (b" " if out else b"") + b"[[" + b", ".join(attrs) + b"]]"
    return out


def _skip_ws_back(src: bytes, i: int) -> int:
    while i >= 0 and src[i] in b" \t\r\n\\":
        i -= 1
    return i


def _decl_start(
    src: bytes, at: int, lo: int, mask: bytearray, stops: bytearray | None = None
) -> int:
    """First byte of the declaration holding 'at': after the last ; { } ,
    or unmatched ( at depth 0 before it, or a preprocessor line ('stops'),
    never before 'lo'."""
    depth = 0
    i = at - 1
    while i >= lo:
        if stops is not None and stops[i]:
            break
        if not mask[i]:
            i -= 1
            continue
        c = src[i]
        if c in b")]":
            depth += 1
        elif c in b"([":
            if depth == 0:
                break
            depth -= 1
        elif depth == 0 and c in b";{},":
            break
        i -= 1
    j = i + 1
    while j < at and (
        src[j] in b" \t\r\n\\" or not mask[j] or (stops is not None and stops[j])
    ):
        j += 1
    return j


def _directive_mask(src: bytes) -> bytearray:
    """1 on every byte of a preprocessor line (continuations included)."""
    stops = bytearray(len(src))
    for a, b in _directive_spans(src):
        stops[a:b] = b"\x01" * (b - a)
    return stops


def _pure(prefix: bytes, macros: set[bytes]) -> bool:
    """Whether 'prefix' is only attributes: [[...]] groups, alignas(...),
    and attribute macros (with their arguments)."""
    i = 0
    while True:
        while i < len(prefix) and prefix[i] in b" \t\r\n\\":
            i += 1
        if i >= len(prefix):
            return True
        if prefix.startswith(b"[[", i):
            end = prefix.find(b"]]", i)
            if end < 0:
                return False
            i = end + 2
            continue
        if prefix.startswith(b"__attribute__", i):
            open_at = prefix.find(b"(", i)
            close = _match_paren(prefix, open_at) if open_at >= 0 else -1
            if close < 0:
                return False
            i = close + 1
            continue
        m = IDENT.match(prefix, i)
        if not m or (m.group() not in macros and m.group() != b"alignas"):
            return False
        i = m.end()
        j = i
        while j < len(prefix) and prefix[j] in b" \t":
            j += 1
        if j < len(prefix) and prefix[j] == 0x28:
            close = _match_paren(prefix, j)
            if close < 0:
                return False
            i = close + 1


def _call_groups(prefix: bytes, macros: set[bytes]) -> list[tuple[int, int]]:
    """Top-level (...) groups in 'prefix' that are not an attribute
    macro's arguments: a declaration's specifiers have none, so more than
    the declarator's own parameter list means the walk back went too far."""
    groups = []
    i = 0
    while i < len(prefix):
        if prefix[i] == 0x28:
            close = _match_paren(prefix, i)
            if close < 0:
                return [(i, len(prefix))]
            j = i - 1
            while j >= 0 and prefix[j] in b" \t":
                j -= 1
            word_end = j + 1
            while j >= 0 and (chr(prefix[j]).isalnum() or prefix[j] == 0x5F):
                j -= 1
            word = prefix[j + 1 : word_end]
            if word not in macros and word != b"alignas":
                groups.append((i, close + 1))
            i = close + 1
        else:
            i += 1
    return groups


def _directive_spans(src: bytes) -> list[tuple[int, int]]:
    return [(m.start(), m.end()) for m in DIRECTIVE.finditer(src)]


def _in(at: int, spans: list[tuple[int, int]]) -> tuple[int, int] | None:
    return next(((a, b) for a, b in spans if a <= at < b), None)


def _move(
    src: bytes, start: int, end: int, to: int, text: bytes
) -> list[tuple[int, int, bytes]]:
    """Edits removing src[start:end] (and the space around it) and
    inserting 'text ' at 'to'."""
    after = end
    while after < len(src) and src[after] in b" \t":
        after += 1
    before = start
    while before > 0 and src[before - 1] in b" \t\r\n":
        before -= 1
    if after < len(src) and src[after] in b";,){":
        cut = (before, end)  # trailing: drop the space before it
    elif after >= len(src) or src[after] == 0x0A:
        line_space = start
        while line_space > 0 and src[line_space - 1] in b" \t":
            line_space -= 1
        cut = (line_space, end)  # last on its line
    else:
        cut = (start, after)
    return [(cut[0], cut[1], b""), (to, to, text + b" ")]


def _apply(src: bytes, edits: list[tuple[int, int, bytes]]) -> bytes:
    out = bytearray(src)
    for start, end, text in sorted(edits, key=lambda e: (e[0], e[1]), reverse=True):
        out[start:end] = text
    return bytes(out)


def _attributes(src: bytes) -> tuple[bytes, list[str]]:
    """Phase 1: each __attribute__((...)) in its standard form and place."""
    mask = code_mask(src, directives_are_code=True)
    spans = _directive_spans(src)
    stops = _directive_mask(src)
    edits: list[tuple[int, int, bytes]] = []
    report: list[str] = []
    for m in re.finditer(rb"__attribute__\s*\(", src):
        at = m.start()
        if not mask[at]:
            continue
        close = _match_paren(src, m.end() - 1)
        inner = src[m.end() : close].strip()
        if close < 0 or not (inner.startswith(b"(") and inner.endswith(b")")):
            continue
        end = close + 1
        line = src.count(b"\n", 0, at) + 1
        directive = _in(at, spans)
        if directive:
            head = DEFINE_HEAD.match(src, directive[0])
            lo = head.end() if head else directive[0]
            start = lo
        else:
            prev = _skip_ws_back(src, at - 1)
            if prev >= 0 and src[prev] == 0x7D and TYPEDEF_NAME_NEXT.match(src, end):
                report.append(f"{line}: {AFTER_BODY}")
                continue
            start = _decl_start(src, at, 0, mask, stops)
        prefix = src[start:at]
        words = IDENT.findall(prefix)
        on_type = bool(words) and words[-1] in TYPE_KEYWORDS
        text = _standard(_split_top(inner[1:-1].replace(b"\\\n", b" ")), on_type)
        groups = _call_groups(prefix, set())
        if on_type or _pure(prefix, set()):
            edits.append((at, end, text))
        elif len(groups) > 1 or (groups and groups[-1][1] != len(prefix.rstrip())):
            report.append(f"{line}: {NOT_A_DECLARATION}")
        else:
            edits += _move(src, at, end, start, text)
    return _apply(src, edits), report


def _macro_uses(src: bytes, macros: set[bytes]) -> bytes:
    """Phase 2: attribute macros after other specifiers move to the front."""
    if not macros:
        return src
    mask = code_mask(src)
    stops = _directive_mask(src)
    edits: list[tuple[int, int, bytes]] = []
    for m in IDENT.finditer(src):
        if m.group() not in macros or not all(mask[m.start() : m.end()]):
            continue
        end = m.end()
        j = end
        while j < len(src) and src[j] in b" \t":
            j += 1
        if j < len(src) and src[j] == 0x28:
            close = _match_paren(src, j)
            if close < 0:
                continue
            end = close + 1
        start = _decl_start(src, m.start(), 0, mask, stops)
        prefix = src[start : m.start()]
        if _pure(prefix, macros) or _call_groups(prefix, macros):
            continue  # already first, or the walk back left the declaration
        edits += _move(src, m.start(), end, start, src[m.start() : end])
    return _apply(src, edits)


def rewrite(src: bytes, macros: set[bytes]) -> tuple[bytes, list[str]]:
    """'src' in standard attribute form, and what is left for a person."""
    out, report = _attributes(src)
    return _macro_uses(out, macros), report


ATTRIBUTE_ONLY = re.compile(
    rb"(?:\s*(?:\[\[(?:[^\]]|\](?!\]))*\]\]|alignas\s*\([^)]*\)))+\s*"
)


def attribute_macros(texts: list[bytes]) -> set[bytes]:
    """Macros whose whole body is attributes ([[...]] / alignas(...))."""
    out: set[bytes] = set()
    for src in texts:
        for d in DIRECTIVE.finditer(src):
            head = DEFINE_HEAD.match(d.group())
            if not head:
                continue
            body = d.group()[head.end() :].replace(b"\\\n", b" ")
            if body.strip() and ATTRIBUTE_ONLY.fullmatch(body):
                name = IDENT.search(d.group(), d.group().find(b"define") + 6).group()
                out.add(name)
    return out


def tree_files(repo: Path) -> list[str]:
    src = repo / "src"
    return sorted(
        str(p.relative_to(repo))
        for p in src.rglob("*")
        if p.suffix in (".c", ".h") and "memshard" not in p.relative_to(src).parts
    )


def run(repo: Path, paths: tuple[str, ...], dry_run: bool) -> int:
    """Convert every file's attributes, then reorder attribute-macro uses
    with the macro set the converted tree defines."""
    targets = list(paths) or tree_files(repo)
    for path in targets:
        if not (repo / path).is_file():
            raise ToolError(f"{path}: no such file")
    phase1: dict[str, bytes] = {}
    total = 0
    for path in targets:
        src = (repo / path).read_bytes()
        out, report = _attributes(src)
        for r in report:
            print(f"  {path}:{r}")
        phase1[path] = out
    texts = [phase1.get(p) or (repo / p).read_bytes() for p in tree_files(repo)]
    macros = attribute_macros(texts)
    for path in targets:
        before = (repo / path).read_bytes()
        out = _macro_uses(phase1[path], macros)
        if out != before:
            total += 1
            print(f"  {path}: changed")
            if not dry_run:
                (repo / path).write_bytes(out)
    verb = "would change" if dry_run else "changed"
    print(
        f"stdattr: {verb} {total} file(s); attribute macros: "
        f"{b' '.join(sorted(macros)).decode() or 'none'}"
    )
    return 0
