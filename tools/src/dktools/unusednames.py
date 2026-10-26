"""``dktools unusednames``: retire ``(void)name;`` silencers for C23 forms.

A silencer (``(void)name;``, ``DK_NOTUSED(name);``, ``UNUSED(name);``)
goes, and the name it silenced gets the C23 form:

- a parameter the body never uses loses its name in the definition (C23
  allows unnamed parameters; the declaration in the header or vtable
  keeps the name), or stays named as ``[[maybe_unused]]`` when the
  comment right above the definition names it (quoted, or an
  identifier-shaped name such as ``bufferLen``; a plain word such as
  "key" in prose is not the parameter);
- a parameter or local used only where a build may drop the code
  (``assert(...)`` arguments, an #if branch), or a local only stored
  to, is declared ``[[maybe_unused]]``;
- a name read in code every build compiles keeps its declaration: the
  silencer was redundant.

Assertions count as code a build may drop, except in test code (inside
``#ifdef DATAKIT_TEST``, or a ``*Test.c`` / ``datakit-test.c`` file),
where ctest.h refuses ``NDEBUG`` so they always compile.

A read is certain unless the innermost call around it is a
function-like macro defined in the sources (other than ``assert``),
which may store into it; a function only reads its arguments. Any
certain read in code every build compiles makes the silencer redundant.

Stores (``x = ...``, ``x += ...``, ``x++``) are not reads; names after
``.`` or ``->`` are members. Code in every #if branch counts as the body
(braces count in the first live branch only; an ``#if 0`` branch is not
code), and an #else the silencer leaves empty goes with it.

Left as written and listed with the reason: a preprocessor line in the
body that names it, a name a macro the body calls uses by itself (ctest.h's
ERR ends with ``err++`` on whatever ``err`` is in scope; macros calling
macros are followed), a name with no parameter or local declaration the
tool reads (a function pointer, a global), a local never used after its
declaration (often a discarded call, ``(void)g();``, which a person
chooses), a local declared together
with other names (``int a, b``), a silencer that is not a statement of
its own (``if (c) (void)a;``), and a silencer outside any definition the
tool reads. Macro bodies are never touched.

Prints each file's count and every refusal; ``--dry-run`` changes nothing.
Exits 0, or 2 for a missing file.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path

from .clex import code_mask
from .gitutil import ToolError
from .retoken import IDENT, sources

NO_DECLARATION = "no parameter or local declaration this tool reads"
SHARED_DECLARATION = "declared together with other names"
IMPLICIT = "a macro the body calls uses the name by itself (as ERR uses err)"
NEVER_USED = "a local never used: by hand ((void)call(); or [[maybe_unused]])"
DIRECTIVE = "a preprocessor line in the body names it"
NOT_A_STATEMENT = "not a statement of its own"
OUTSIDE = "not inside a definition this tool reads"

SILENCER = re.compile(
    rb"\(\s*void\s*\)\s*([A-Za-z_]\w*)\s*;"
    rb"|\b(?:DK_NOTUSED|UNUSED)\s*\(\s*([A-Za-z_]\w*)\s*\)\s*;"
)
DIRECTIVE_LINE = re.compile(rb"(?m)^[ \t]*#(?:[^\n]|\\\n)*")
CONDITIONAL = re.compile(
    rb"(?m)^[ \t]*(#)[ \t]*(ifdef|ifndef|if|elifdef|elifndef|elif|else|endif)\b"
    rb"[ \t]*(\S*)"
)
#: words that end a type, so a parameter ending in one has no name
TYPE_WORDS = frozenset(
    b"void char short int long float double signed unsigned bool _Bool "
    b"const volatile restrict".split()
)
NOT_FUNCTIONS = frozenset(b"if for while switch sizeof __attribute__ return".split())


@dataclass(frozen=True)
class Refusal:
    line: int
    name: str
    reason: str
    detail: str = ""  # such as the macro that uses the name


@dataclass
class _Param:
    name: bytes | None
    start: int  # first byte of the parameter text (after leading space)
    name_start: int
    name_end: int


@dataclass
class _Function:
    name_start: int
    params: list[_Param]
    body_open: int
    body_close: int


@dataclass
class _Branch:
    start: int  # brace depth at the #if
    first: int | None  # depth at the end of the first live branch
    dead: bool  # inside an '#if 0' branch
    inherited: bool  # dead because an enclosing branch is


def _prev_code(src: bytes, mask: bytearray, i: int) -> int:
    """Index of the last code byte before 'i' that is not whitespace, or -1."""
    i -= 1
    while i >= 0 and (not mask[i] or src[i] in b" \t\r\n"):
        i -= 1
    return i


def _match_back(src: bytes, mask: bytearray, close: int) -> int:
    """Index of the '(' that the ')' at 'close' closes, or -1."""
    depth = 0
    for i in range(close, -1, -1):
        if not mask[i]:
            continue
        if src[i] == 0x29:  # )
            depth += 1
        elif src[i] == 0x28:  # (
            depth -= 1
            if depth == 0:
                return i
    return -1


def _params(src: bytes, mask: bytearray, open_: int, close: int) -> list[_Param]:
    """The parameters between the parentheses at 'open_' and 'close'."""
    pieces = []
    depth = 0
    start = open_ + 1
    for i in range(open_ + 1, close):
        if not mask[i]:
            continue
        c = src[i]
        if c in b"([":
            depth += 1
        elif c in b")]":
            depth -= 1
        elif c == 0x2C and depth == 0:  # ,
            pieces.append((start, i))
            start = i + 1
    pieces.append((start, close))
    out = []
    for lo, hi in pieces:
        while lo < hi and src[lo] in b" \t\r\n":
            lo += 1
        text = src[lo:hi]
        param = _Param(None, lo, -1, -1)
        if b"(" not in text:
            # the name is the last word before any [bound]s
            end = hi
            while True:
                j = _prev_code(src, mask, end)
                if j < lo or src[j] != 0x5D:  # ]
                    break
                depth = 0
                while j >= lo:
                    if src[j] == 0x5D:
                        depth += 1
                    elif src[j] == 0x5B:
                        depth -= 1
                        if depth == 0:
                            break
                    j -= 1
                end = j
            words = [
                m
                for m in IDENT.finditer(src, lo, end)
                if all(mask[m.start() : m.end()])
            ]
            if len(words) >= 2 or (
                words and _prev_code(src, mask, words[-1].start()) >= lo
            ):
                last = words[-1]
                if last.group() not in TYPE_WORDS:
                    param = _Param(last.group(), lo, last.start(), last.end())
        out.append(param)
    return out


def _conditionals(src: bytes) -> dict[int, tuple[bytes, bool]]:
    """Start offset -> (keyword, whether it is '#if 0') of every #if /
    #elif / #else / #endif line."""
    directive_mask = code_mask(src, directives_are_code=True)
    return {
        m.start(1): (m.group(2), m.group(2) == b"if" and m.group(3) == b"0")
        for m in CONDITIONAL.finditer(src)
        if directive_mask[m.start(1)]
    }


#: conditions that select test-only code, where ctest.h forbids NDEBUG
TEST_GUARDS = frozenset((b"DATAKIT_TEST", b"defined(DATAKIT_TEST)"))
#: test sources, compiled only into the test binary
TEST_FILE = re.compile(r"(?:Test\.c|datakit-test\.c)$")


def _test_ranges(src: bytes) -> list[tuple[int, int]]:
    """The ranges inside a live '#if(def) DATAKIT_TEST' branch, where
    assertions are always compiled (ctest.h refuses NDEBUG there)."""
    directive_mask = code_mask(src, directives_are_code=True)
    out: list[tuple[int, int]] = []
    stack: list[list] = []  # [is the test guard, branch start or None]
    for m in CONDITIONAL.finditer(src):
        if not directive_mask[m.start(1)]:
            continue
        keyword, token = m.group(2), m.group(3)
        if keyword in (b"if", b"ifdef"):
            guard = token in TEST_GUARDS
            stack.append([guard, m.end() if guard else None])
        elif keyword == b"ifndef":
            stack.append([False, None])
        elif not stack:
            continue
        elif keyword == b"endif":
            guard, start = stack.pop()
            if guard and start is not None:
                out.append((start, m.start()))
        else:  # elif / else: past the test branch
            guard, start = stack[-1]
            if guard and start is not None:
                out.append((start, m.start()))
                stack[-1][1] = None
    return out


def _functions(src: bytes, mask: bytearray) -> list[_Function]:
    """Every top-level function definition: '...name(params) {...}'.

    Braces count in the first live branch of each #if only, so a line
    repeated across branches (`if (a) {` / `#else` / `if (b) {`) opens
    one block, as it does in every build. An `#if 0` branch (and all it
    holds) is not code; its #else is."""
    out = []
    depth = 0
    current: _Function | None = None
    conditionals = _conditionals(src)
    branches: list[_Branch] = []
    for i, c in enumerate(src):
        found = conditionals.get(i)
        if found is not None:
            keyword, zero = found
            outer_dead = bool(branches) and branches[-1].dead
            if keyword.startswith(b"if"):
                branches.append(_Branch(depth, None, zero or outer_dead, outer_dead))
            elif branches and keyword == b"endif":
                branch = branches.pop()
                if branch.first is not None:
                    depth = branch.first
            elif branches:  # elif / else: the next branch starts over
                branch = branches[-1]
                if branch.dead and not branch.inherited:
                    branch.dead = False  # the branch after '#if 0' is the live one
                elif branch.first is None and not branch.dead:
                    branch.first = depth
                depth = branch.start
            continue
        if not mask[i] or (branches and branches[-1].dead):
            continue
        if c == 0x7B:  # {
            if depth == 0:
                j = _prev_code(src, mask, i)
                if j >= 0 and src[j] == 0x29:
                    open_ = _match_back(src, mask, j)
                    k = _prev_code(src, mask, open_) if open_ > 0 else -1
                    word = None
                    if k >= 0:
                        s = k
                        while s > 0 and (
                            chr(src[s - 1]).isalnum() or src[s - 1] == 0x5F
                        ):
                            s -= 1
                        word = src[s : k + 1]
                        if (
                            re.fullmatch(rb"[A-Za-z_]\w*", word)
                            and word not in NOT_FUNCTIONS
                        ):
                            current = _Function(s, _params(src, mask, open_, j), i, -1)
            depth += 1
        elif c == 0x7D:  # }
            depth -= 1
            if depth == 0 and current is not None:
                current.body_close = i
                out.append(current)
                current = None
            if depth < 0:
                depth = 0
    return out


def _comment_names(comment: bytes, name: bytes) -> bool:
    """Whether 'comment' refers to the parameter 'name': quoted ('key',
    `key`, "key"), or the name is identifier-shaped (an underscore, a
    digit or an inner capital, as bufferLen) so it is not a plain word."""
    if re.search(rb"['`\"]" + re.escape(name) + rb"['`\"]", comment):
        return True
    shaped = re.search(rb"_|\d|.[A-Z]", name) is not None
    return shaped and re.search(rb"\b" + re.escape(name) + rb"\b", comment) is not None


def _doc_comment(src: bytes, fn: _Function) -> bytes:
    """The comment directly above the definition (above its return-type
    line when the type sits on a line of its own), or b''."""
    line_start = src.rfind(b"\n", 0, fn.name_start) + 1
    before = src[:line_start].rstrip()
    if before and not before.endswith((b";", b"}", b"*/", b"{")):
        last = before.rfind(b"\n") + 1
        if not before[last:].lstrip().startswith((b"#", b"//")):
            before = before[:last].rstrip()
    if before.endswith(b"*/"):
        return before[before.rfind(b"/*") :]
    lines = before.split(b"\n")
    comment = []
    while lines and lines[-1].lstrip().startswith(b"//"):
        comment.append(lines.pop())
    return b"\n".join(comment)


def unname(
    src: bytes, test_file: bool = False, macros: Macros | None = None
) -> tuple[bytes, int, list[Refusal]]:
    """'src' with each silencer retired (see the module docstring), how many
    names that covered, and the silencers left. 'test_file': the whole
    source is test code, where assertions always compile. 'macros': the
    function-like macros other files define ('src's own are added)."""
    all_macros = {**(macros or {}), **macro_names(src)}
    mask = code_mask(src)
    functions = _functions(src, mask)
    conditionals = _conditionals(src)
    test_ranges = _test_ranges(src)
    directive_mask = code_mask(src, directives_are_code=True)
    silencers = [m for m in SILENCER.finditer(src) if all(mask[m.start() : m.end()])]
    edits: list[tuple[int, int, bytes]] = []
    refusals: list[Refusal] = []
    count = 0

    def refuse(m: re.Match[bytes], reason: str, detail: str = "") -> None:
        name = (m.group(1) or m.group(2)).decode()
        line = src.count(b"\n", 0, m.start()) + 1
        refusals.append(Refusal(line, name, reason, detail))

    by_function: dict[int, list[re.Match[bytes]]] = {}
    for m in silencers:
        owner = next(
            (
                n
                for n, fn in enumerate(functions)
                if fn.body_open < m.start() < fn.body_close
            ),
            None,
        )
        if owner is None:
            refuse(m, OUTSIDE)
            continue
        by_function.setdefault(owner, []).append(m)

    for n, found in by_function.items():
        fn = functions[n]
        # the code of the body's preprocessor lines (their comments blanked)
        directives = b"\n".join(
            bytes(
                b if directive_mask[d.start() + k] else 0x20
                for k, b in enumerate(d.group())
            )
            for d in DIRECTIVE_LINE.finditer(src, fn.body_open, fn.body_close)
        )
        params = {p.name: p for p in fn.params if p.name}
        spans = [(m.start(), m.end()) for m in found]
        guarded = _guarded(conditionals, fn.body_open, fn.body_close)
        # names the macros this body calls reach in its scope by themselves
        implicit = _implicit_uses(
            {
                t.group()
                for t in IDENT.finditer(src, fn.body_open, fn.body_close)
                if mask[t.start()]
            },
            all_macros,
        )
        doc = _doc_comment(src, fn)
        done: set[tuple[bytes, int]] = set()
        # code an NDEBUG or #if build may drop; assertions always compile in
        # test code (ctest.h refuses NDEBUG there)
        optional = list(guarded)
        if not (test_file or _inside(fn.name_start, test_ranges)):
            optional = optional + _assert_spans(src, mask, fn.body_open, fn.body_close)
        for m in found:
            name = m.group(1) or m.group(2)
            prev = _prev_code(src, mask, m.start())
            if src[prev] not in b";{}:":
                refuse(m, NOT_A_STATEMENT)
                continue
            word = rb"\b" + re.escape(name) + rb"\b"
            if re.search(word, directives):
                refuse(m, DIRECTIVE)
                continue
            if name in implicit:
                refuse(m, IMPLICIT, f"via {implicit[name].decode()}")
                continue
            decl = None
            if name not in params:
                decl = _declaration(src, mask, fn.body_open, m.start(), name)
                if decl is None:
                    refuse(m, NO_DECLARATION)
                    continue
                if decl.shared:
                    refuse(m, SHARED_DECLARATION)
                    continue
            # a parameter's scope is the body; a local's, its own block
            lo, hi = (
                (fn.body_open, fn.body_close)
                if decl is None
                else (decl.start, decl.scope_end)
            )
            own = -1 if decl is None else decl.declarator
            shadows = _shadowed(src, mask, name, lo, hi, own)
            uses = [
                t.start()
                for t in IDENT.finditer(src, lo, hi)
                if t.group() == name
                and all(mask[t.start() : t.end()])
                and not _is_member(src, mask, t.start())
                and not any(a <= t.start() < b for a, b in spans)
                and t.start() != own
                and not _inside(t.start(), shadows)
            ]
            reads = [at for at in uses if not _is_write(src, mask, at, len(name))]
            if decl is not None and not uses and not decl.marked:
                refuse(m, NEVER_USED)
                continue
            edits.append(_statement_removal(src, m))
            # one decision per declaration (a parameter, or each local of
            # the name: blocks redeclare names)
            key = (name, -1 if decl is None else decl.declarator)
            if key in done:
                continue
            done.add(key)
            count += 1
            if any(
                not _inside(at, optional)
                and not _in_macro_call(src, mask, at, fn.body_open, all_macros)
                for at in reads
            ):
                # a certain read (not a call argument, which a macro may
                # store into) in every build: the silencer was redundant
                continue
            if decl is not None:
                if not decl.marked:
                    edits.append((decl.start, decl.start, b"[[maybe_unused]] "))
                continue
            p = params[name]
            if uses or _comment_names(doc, name):
                edits.append((p.start, p.start, b"[[maybe_unused]] "))
            else:
                cut = p.name_start
                while cut > p.start and src[cut - 1] in b" \t\r\n":
                    cut -= 1
                edits.append((cut, p.name_end, b""))

    out = bytearray(src)
    for start, end, text in sorted(edits, reverse=True):
        out[start:end] = text
    refusals.sort(key=lambda r: r.line)
    return bytes(out), count, refusals


@dataclass(frozen=True)
class _Declaration:
    start: int  # first byte of the declaration statement
    declarator: int  # offset of the declared name
    shared: bool  # declares other names too (int a, b)
    marked: bool  # already [[maybe_unused]]
    scope_end: int  # the '}' closing the block it is declared in


#: a declaration's text before its name: an optional attribute, then type
#: words, macro calls such as cleaner(f) and '*'s
#: (possessive throughout: a long prefix that is not a declaration must
#: fail in linear time, not by trying every split of every word)
DECLARATION_PREFIX = re.compile(
    rb"(\[\[\s*+maybe_unused\s*+\]\]\s*+)?"
    rb"(?:\[\[[^\]]*+\]\]\s*+)*+"
    rb"(?:[A-Za-z_]\w*+\s*+(?:\([^()]*+\)\s*+)?|\*\s*+)++"
)
STATEMENT_WORDS = frozenset(
    b"return case goto else do if while for switch break continue sizeof".split()
)


def _declaration(
    src: bytes, mask: bytearray, lo: int, before: int, name: bytes
) -> _Declaration | None:
    """The nearest declaration of the local 'name' in [lo, before) whose
    block still holds 'before'."""
    found = None
    for t in IDENT.finditer(src, lo, before):
        if t.group() != name or not all(mask[t.start() : t.end()]):
            continue
        decl = _declarator(src, mask, t.start(), t.end(), lo)
        if decl is not None and decl.scope_end >= before:  # not a closed sibling's
            found = decl
    return found


def _declarator(
    src: bytes, mask: bytearray, at: int, end: int, lo: int
) -> _Declaration | None:
    """The declaration whose declared name is the token [at, end): a local,
    or a field of a struct defined in the body; None for any other use."""
    nxt = _next_code(src, mask, end)
    if nxt < 0 or src[nxt] not in b"=;[,":
        return None
    if src[nxt] == 0x3D and src[nxt + 1 : nxt + 2] == b"=":  # ==
        return None
    boundary = _prev_code(src, mask, at)
    while boundary >= lo and src[boundary] not in b";{}":
        boundary = _prev_code(src, mask, boundary)
    start = _next_code(src, mask, boundary + 1)
    # the code before the name (comments and directives between blanked)
    prefix = bytes(src[k] if mask[k] else 0x20 for k in range(start, at))
    words = IDENT.findall(prefix)
    if not words or words[0] in STATEMENT_WORDS:
        return None
    m = DECLARATION_PREFIX.fullmatch(prefix.rstrip())
    if m:
        shared = _declares_more(src, mask, end)
    else:
        # a later name of 'int a = 1, b': the text before the first ',' or
        # '=' declares 'a'
        head = re.split(rb"[,=]", prefix, maxsplit=1)[0].rstrip()
        lead = DECLARATION_PREFIX.fullmatch(head)
        if b"," not in prefix or not lead:
            return None
        m, shared = lead, True
    return _Declaration(
        start, at, shared, m.group(1) is not None, _block_end(src, mask, at, lo)
    )


def _shadowed(
    src: bytes, mask: bytearray, name: bytes, lo: int, hi: int, own: int
) -> list[tuple[int, int]]:
    """The ranges of [lo, hi) where another declaration of 'name' (a
    shadowing local or a struct field; not the one at 'own') is in force."""
    out = []
    for t in IDENT.finditer(src, lo, hi):
        if t.group() != name or t.start() == own or not all(mask[t.start() : t.end()]):
            continue
        decl = _declarator(src, mask, t.start(), t.end(), lo)
        if decl is not None:
            out.append((t.start(), decl.scope_end))
    return out


def _block_end(src: bytes, mask: bytearray, at: int, lo: int) -> int:
    """The '}' closing the innermost block that holds 'at' (lo: the
    function body's '{')."""
    depth = 0
    i = at - 1
    while i > lo:
        if mask[i]:
            if src[i] == 0x7D:  # }
                depth += 1
            elif src[i] == 0x7B:  # {
                if depth == 0:
                    break
                depth -= 1
        i -= 1
    depth = 0
    for j in range(i, len(src)):
        if not mask[j]:
            continue
        if src[j] == 0x7B:
            depth += 1
        elif src[j] == 0x7D:
            depth -= 1
            if depth == 0:
                return j
    return len(src)


def _declares_more(src: bytes, mask: bytearray, at: int) -> bool:
    """Whether a top-level ',' comes before the ';' ending the statement."""
    depth = 0
    for i in range(at, len(src)):
        if not mask[i]:
            continue
        c = src[i]
        if c in b"([{":
            depth += 1
        elif c in b")]}":
            depth -= 1
        elif c == 0x2C and depth == 0:
            return True
        elif c == 0x3B and depth == 0:
            return False
    return False


def _next_code(src: bytes, mask: bytearray, i: int) -> int:
    """Index of the first code byte at or after 'i' that is not whitespace."""
    while i < len(src) and (not mask[i] or src[i] in b" \t\r\n"):
        i += 1
    return i if i < len(src) else -1


ASSIGNMENTS = (b"<<=", b">>=", b"+=", b"-=", b"*=", b"/=", b"%=", b"&=", b"|=", b"^=")


def _is_write(src: bytes, mask: bytearray, at: int, length: int) -> bool:
    """Whether the name at 'at' is only stored to (x = ..., x += ...,
    x++, --x), which does not use its value."""
    nxt = _next_code(src, mask, at + length)
    after = src[nxt : nxt + 3] if nxt >= 0 else b""
    prev = _prev_code(src, mask, at)
    if prev >= 0 and src[prev] == 0x2A:  # *x = ...: a store through x reads x
        return False
    if after.startswith(b"=") and not after.startswith(b"=="):
        return True
    if after.startswith(ASSIGNMENTS) or after.startswith((b"++", b"--")):
        return True
    return prev > 0 and src[prev - 1 : prev + 1] in (b"++", b"--")


ASSERT_CALL = re.compile(rb"\bassert\s*\(")


def _assert_spans(
    src: bytes, mask: bytearray, lo: int, hi: int
) -> list[tuple[int, int]]:
    """The argument ranges of the assert(...) calls in [lo, hi)."""
    out = []
    for m in ASSERT_CALL.finditer(src, lo, hi):
        if not all(mask[m.start() : m.end()]):
            continue
        depth = 0
        for i in range(m.end() - 1, hi):
            if not mask[i]:
                continue
            if src[i] == 0x28:
                depth += 1
            elif src[i] == 0x29:
                depth -= 1
                if depth == 0:
                    out.append((m.end(), i))
                    break
    return out


#: words that open parentheses without making a call
NOT_CALLS = frozenset(b"if while for switch return sizeof alignof typeof".split())


#: function-like macros that only read their arguments
READING_MACROS = frozenset((b"assert",))
#: a #define: name, the parameter list of a function-like macro, the body
#: (continuation lines included)
DEFINE = re.compile(
    rb"(?m)^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)(\(([^)]*)\))?((?:[^\n\\]|\\.)*)",
    re.DOTALL,
)
NAME_TOKEN = re.compile(rb"[A-Za-z_]\w*")
MACRO_KEYWORDS = frozenset((b"__VA_ARGS__", b"__VA_OPT__", b"defined"))

Macros = dict[bytes, frozenset[bytes]]


def macro_names(src: bytes) -> Macros:
    """Each macro 'src' defines -> the identifiers its body uses that are
    not its own parameters (a name it reaches in the caller's scope)."""
    out: Macros = {}
    for m in DEFINE.finditer(src):
        params = {p.strip() for p in (m.group(3) or b"").split(b",")}
        text = m.group(4)
        mask = code_mask(text)  # words in the body's strings and comments are not names
        words = {
            t.group()
            for t in NAME_TOKEN.finditer(text)
            if all(mask[t.start() : t.end()])
        }
        body = words - params - MACRO_KEYWORDS
        body.discard(m.group(1))
        out[m.group(1)] = frozenset(body)
    return out


def _implicit_uses(body_names: set[bytes], macros: Macros) -> dict[bytes, bytes]:
    """The names the macros called in a body use by themselves -> the
    macro the body calls that reaches each (following macros that call
    macros)."""
    seen: set[bytes] = set()
    todo = [(n, n) for n in sorted(body_names) if n in macros]
    found: dict[bytes, bytes] = {}
    while todo:
        name, root = todo.pop()
        if name in seen:
            continue
        seen.add(name)
        for used in sorted(macros[name]):
            found.setdefault(used, root)
            if used in macros:
                todo.append((used, root))
    return found


def _in_macro_call(
    src: bytes, mask: bytearray, at: int, lo: int, macros: Macros
) -> bool:
    """Whether the innermost call around the name at 'at' is a
    function-like macro, which may store into it (a function only reads
    its arguments; conditions and groupings are looked through)."""
    depth = 0
    i = at - 1
    while i >= lo:
        if mask[i]:
            c = src[i]
            if c == 0x29:  # )
                depth += 1
            elif c == 0x28:  # (
                if depth == 0:
                    j = _prev_code(src, mask, i)
                    s = j
                    while s > 0 and (chr(src[s - 1]).isalnum() or src[s - 1] == 0x5F):
                        s -= 1
                    word = src[s : j + 1]
                    if re.fullmatch(rb"[A-Za-z_]\w*", word) and word not in NOT_CALLS:
                        return word in macros and word not in READING_MACROS
                    i = j  # a condition or grouping: look further out
                    continue
                depth -= 1
            elif c in b";{}" and depth == 0:
                return False
        i -= 1
    return False


def _is_member(src: bytes, mask: bytearray, at: int) -> bool:
    """Whether the name at 'at' follows '.' or '->' (a member or a
    designator, not the variable)."""
    j = _prev_code(src, mask, at)
    return j >= 0 and (
        src[j] == 0x2E or (src[j] == 0x3E and j > 0 and src[j - 1] == 0x2D)
    )


def _guarded(
    conditionals: dict[int, tuple[bytes, bool]], lo: int, hi: int
) -> list[tuple[int, int]]:
    """The ranges of [lo, hi) inside an #if that opens in [lo, hi)."""
    out = []
    nesting = 0
    start = lo
    for at in sorted(k for k in conditionals if lo <= k < hi):
        keyword = conditionals[at][0]
        if keyword.startswith(b"if"):
            if nesting == 0:
                start = at
            nesting += 1
        elif keyword == b"endif" and nesting:
            nesting -= 1
            if nesting == 0:
                out.append((start, at))
    if nesting:
        out.append((start, hi))
    return out


def _inside(at: int, ranges: list[tuple[int, int]]) -> bool:
    return any(a <= at < b for a, b in ranges)


ELSE_LINE = re.compile(rb"[ \t]*#[ \t]*else\b[^\n]*\n")
ENDIF_LINE = re.compile(rb"[ \t]*#[ \t]*endif\b")


def _statement_removal(src: bytes, m: re.Match[bytes]) -> tuple[int, int, bytes]:
    """The edit that deletes the silencer: its whole line when it stands
    alone there (with the #else above it when that leaves the branch
    empty), else the statement and the spaces after it."""
    line_start = src.rfind(b"\n", 0, m.start()) + 1
    line_end = src.find(b"\n", m.end())
    line_end = len(src) if line_end < 0 else line_end + 1
    if not src[line_start : m.start()].strip() and not src[m.end() : line_end].strip():
        prev_start = src.rfind(b"\n", 0, max(line_start - 1, 0)) + 1
        if (
            line_start > 0
            and ELSE_LINE.fullmatch(src, prev_start, line_start)
            and ENDIF_LINE.match(src, line_end)
        ):
            return prev_start, line_end, b""
        return line_start, line_end, b""
    end = m.end()
    while end < len(src) and src[end] in b" \t":
        end += 1
    return m.start(), end, b""


def run(repo: Path, paths: tuple[str, ...], dry_run: bool) -> int:
    targets = list(paths) or sources(repo)
    # function-like macros live in the headers, datakit's and the vendored
    # ones it includes (each file's own are added by unname); more names
    # here only make the tool more careful
    headers = [h for top in ("src", "deps") for h in sorted((repo / top).rglob("*.h"))]
    macros: Macros = {}
    for h in headers:
        macros.update(macro_names(h.read_bytes()))
    total = 0
    refused = 0
    for path in targets:
        target = repo / path
        if not target.is_file():
            raise ToolError(f"{path}: no such file")
        src = target.read_bytes()
        out, count, refusals = unname(src, bool(TEST_FILE.search(path)), macros)
        for r in refusals:
            detail = f" ({r.detail})" if r.detail else ""
            print(f"  {path}:{r.line}: {r.name}: {r.reason}{detail}")
        refused += len(refusals)
        if count:
            total += count
            print(f"  {path}: {count}")
            if not dry_run:
                target.write_bytes(out)
    verb = "would retire" if dry_run else "retired"
    print(f"unusednames: {verb} the silencers of {total} name(s); {refused} left")
    return 0
