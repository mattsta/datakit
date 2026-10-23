"""Atomic-ordering audit for datakit.

Every atomic operation in ``src/`` (vendored ``memshard`` excepted) must
say why its memory order is what it is, in a structured comment that
this module checks against the code::

    /* @sync: release arena.retire */
    /* @sync: acquire mailbox.slot; release mailbox.ack */
    /* @sync: seq_cst gate.count */
    /* @sync: relaxed counter */
    /* @sync: local timing */

Grammar (validated; malformed tags are violations)::

    @sync: CLAUSE (; CLAUSE)*
    CLAUSE := (release|acquire|acq_rel|seq_cst) EDGE
            | relaxed (counter|seed|owner|init|hint)
            | local (timing|signal)
    EDGE   := [A-Za-z_][A-Za-z0-9_.]*

An EDGE names one happens-before relation of the program, the same name
at every operation taking part in it (``module.purpose``).
  release  the operation publishes along EDGE (a release store or RMW,
           or a release fence).
  acquire  the operation observes EDGE (an acquire load or RMW, or an
           acquire fence).
  acq_rel  both, on one RMW.
  seq_cst  the argument along EDGE needs the single total order (the
           store-buffering pattern); every operation on EDGE is seq_cst.
  relaxed  no ordering is claimed, and the reason is one of:
           counter  a count or statistic; nothing is published through it
           seed     the load only seeds a CAS, which compares the current
                    value and carries its own order
           owner    only one thread can reach the object here (under its
                    lock, a single writer reading back its own value)
           init     before the object is shared or after every other
                    user is joined
           hint     a stale value is harmless: an ordered operation
                    re-checks the decision
  local    the order is for this thread's own non-memory effects (a
           timing barrier, a signal handler), not for another thread.

What ``check`` proves, per operation:
  - it has exactly one tag (a tag that no operation claims is itself a
    violation, so a rotted tag cannot read as clean);
  - its memory order is a constant, and valid for the operation (no
    release load, no acquire store, CAS failure order no stronger than
    success, no ``consume``);
  - the order provides exactly what the tag claims: weaker is an
    UNDER-ORDERED violation (the claimed edge does not exist), stronger
    is an OVER-ORDERED one (an ordering nobody justified).  ``local``
    claims whatever order the operation has.
And per EDGE, across the tree:
  - it has a publishing side (release, acq_rel or seq_cst) and an
    observing side (acquire, acq_rel or seq_cst): an edge with only one
    side synchronizes nothing;
  - a seq_cst edge is seq_cst at every operation on it.

Implicit-order forms (``atomic_load(p)``, ``__sync_*``) count as
seq_cst.  Plain operators on ``_Atomic`` objects (``x++``, ``x = v``) are
seq_cst too but cannot be found lexically; clang builds reject them
instead, through ``-Watomic-implicit-seq-cst`` (src/CMakeLists.txt).

Discovery is lexical (comments and string literals stripped, line
continuations joined), so operations inside macro definitions are
audited at the definition.  A tag attaches to the nearest operation
after it within the same statement (no ``;``, ``{``, ``}`` or other
atomic operation in between), or, trailing, to the operation whose call
closes on the tag's line.
"""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path

#: Directories under src/ that are vendored and out of audit scope.
EXCLUDE_DIRS = ("memshard",)

ACQ, REL, SC = "acquire", "release", "seq_cst"

#: Memory order spellings -> the guarantees they carry.
ORDERS: dict[str, frozenset[str]] = {}
for _prefix, _names in (
    (
        "memory_order_",
        ("relaxed", "consume", "acquire", "release", "acq_rel", "seq_cst"),
    ),
    ("__ATOMIC_", ("RELAXED", "CONSUME", "ACQUIRE", "RELEASE", "ACQ_REL", "SEQ_CST")),
):
    for _n in _names:
        _k = _n.lower()
        ORDERS[_prefix + _n] = {
            "relaxed": frozenset(),
            "consume": frozenset({"consume"}),
            "acquire": frozenset({ACQ}),
            "release": frozenset({REL}),
            "acq_rel": frozenset({ACQ, REL}),
            "seq_cst": frozenset({ACQ, REL, SC}),
        }[_k]

#: What each kind of operation can provide.
CAPABILITY = {
    "load": frozenset({ACQ, SC}),
    "store": frozenset({REL, SC}),
    "rmw": frozenset({ACQ, REL, SC}),
    "cas": frozenset({ACQ, REL, SC}),
    "fence": frozenset({ACQ, REL, SC}),
}

#: name -> (kind, order argument indices; () means implicitly seq_cst).
OPS: dict[str, tuple[str, tuple[int, ...]]] = {
    "atomic_load_explicit": ("load", (1,)),
    "atomic_store_explicit": ("store", (2,)),
    "atomic_exchange_explicit": ("rmw", (2,)),
    "atomic_compare_exchange_weak_explicit": ("cas", (3, 4)),
    "atomic_compare_exchange_strong_explicit": ("cas", (3, 4)),
    "atomic_flag_test_and_set_explicit": ("rmw", (1,)),
    "atomic_flag_clear_explicit": ("store", (1,)),
    "atomic_thread_fence": ("fence", (0,)),
    "atomic_signal_fence": ("fence", (0,)),
    "atomic_load": ("load", ()),
    "atomic_store": ("store", ()),
    "atomic_exchange": ("rmw", ()),
    "atomic_compare_exchange_weak": ("cas", ()),
    "atomic_compare_exchange_strong": ("cas", ()),
    "atomic_flag_test_and_set": ("rmw", ()),
    "atomic_flag_clear": ("store", ()),
    "__atomic_load_n": ("load", (1,)),
    "__atomic_load": ("load", (2,)),
    "__atomic_store_n": ("store", (2,)),
    "__atomic_store": ("store", (2,)),
    "__atomic_exchange_n": ("rmw", (2,)),
    "__atomic_exchange": ("rmw", (3,)),
    "__atomic_compare_exchange_n": ("cas", (4, 5)),
    "__atomic_compare_exchange": ("cas", (4, 5)),
    "__atomic_test_and_set": ("rmw", (1,)),
    "__atomic_clear": ("store", (1,)),
    "__atomic_thread_fence": ("fence", (0,)),
    "__atomic_signal_fence": ("fence", (0,)),
    "__sync_synchronize": ("fence", ()),
}
for _op in ("add", "sub", "or", "xor", "and"):
    OPS[f"atomic_fetch_{_op}_explicit"] = ("rmw", (2,))
    OPS[f"atomic_fetch_{_op}"] = ("rmw", ())
for _op in ("add", "sub", "or", "xor", "and", "nand"):
    OPS[f"__atomic_{_op}_fetch"] = ("rmw", (2,))
    OPS[f"__atomic_fetch_{_op}"] = ("rmw", (2,))
    OPS[f"__sync_fetch_and_{_op}"] = ("rmw", ())
    OPS[f"__sync_{_op}_and_fetch"] = ("rmw", ())
OPS["__sync_bool_compare_and_swap"] = ("cas", ())
OPS["__sync_val_compare_and_swap"] = ("cas", ())
OPS["__sync_lock_test_and_set"] = ("rmw", ())

ORDERED_ROLES = {
    "release": frozenset({REL}),
    "acquire": frozenset({ACQ}),
    "acq_rel": frozenset({ACQ, REL}),
    "seq_cst": frozenset({ACQ, REL, SC}),
}
RELAXED_REASONS = ("counter", "seed", "owner", "init", "hint")
LOCAL_REASONS = ("timing", "signal")

EDGE_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_.]*\Z")
#: The whole comment is the tag: prose goes in a comment of its own.
TAG_RE = re.compile(r"\A(?:/\*|//)\s*@sync:(?P<body>.*?)(?:\*/)?\s*\Z", re.S)
TAG_LOOSE_RE = re.compile(r"@sync\b")


@dataclass(frozen=True)
class Clause:
    role: str
    arg: str


@dataclass
class Tag:
    path: str
    line: int
    offset: int
    text: str
    clauses: list[Clause] = field(default_factory=list)
    error: str | None = None
    used: bool = False


@dataclass
class Site:
    path: str
    line: int
    op: str
    orders: list[str]
    tag: Tag | None = None
    problems: list[str] = field(default_factory=list)


@dataclass
class Violation:
    path: str
    line: int
    message: str


@dataclass
class AuditResult:
    sites: list[Site] = field(default_factory=list)
    tags: list[Tag] = field(default_factory=list)
    violations: list[Violation] = field(default_factory=list)

    @property
    def clean(self) -> bool:
        return not self.violations

    def edges(self) -> dict[str, list[tuple[Site, str]]]:
        out: dict[str, list[tuple[Site, str]]] = {}
        for s in self.sites:
            if s.tag is None or s.tag.error:
                continue
            for c in s.tag.clauses:
                if c.role in ORDERED_ROLES:
                    out.setdefault(c.arg, []).append((s, c.role))
        return dict(sorted(out.items()))


# -- lexing ------------------------------------------------------------


@dataclass(frozen=True, slots=True)
class Token:
    text: str
    offset: int
    line: int


@dataclass(frozen=True, slots=True)
class Comment:
    text: str
    offset: int
    line: int


_IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_NUMBER_RE = re.compile(r"\.?[0-9](?:[eEpP][+-]|[A-Za-z0-9_.])*")


def lex(src: str) -> tuple[list[Token], list[Comment]]:
    """Tokens and comments of C source, with line numbers.  Line
    continuations are whitespace; string and character literals become
    one opaque token."""
    tokens: list[Token] = []
    comments: list[Comment] = []
    i, n, line = 0, len(src), 1
    while i < n:
        ch = src[i]
        if ch == "\n":
            line += 1
            i += 1
        elif ch in " \t\r\f\v":
            i += 1
        elif ch == "\\" and src.startswith("\n", i + 1):
            line += 1
            i += 2
        elif src.startswith("/*", i):
            end = src.find("*/", i + 2)
            end = n if end < 0 else end + 2
            comments.append(Comment(src[i:end], i, line))
            line += src.count("\n", i, end)
            i = end
        elif src.startswith("//", i):
            end = src.find("\n", i)
            end = n if end < 0 else end
            comments.append(Comment(src[i:end], i, line))
            i = end
        elif ch in "\"'":
            j = i + 1
            while j < n and src[j] != ch and src[j] != "\n":
                j += 2 if src[j] == "\\" else 1
            j = min(j + 1, n)
            tokens.append(Token(src[i:j], i, line))
            line += src.count("\n", i, j)
            i = j
        else:
            m = _IDENT_RE.match(src, i) or _NUMBER_RE.match(src, i)
            j = m.end() if m else i + 1
            tokens.append(Token(src[i:j], i, line))
            i = j
    return tokens, comments


# -- tags --------------------------------------------------------------


def parse_tag(text: str) -> tuple[list[Clause], str | None]:
    m = TAG_RE.search(text)
    if not m:
        return [], "malformed @sync tag (the comment must hold only the tag)"
    body = m.group("body").strip()
    clauses: list[Clause] = []
    for raw in body.split(";"):
        words = raw.split()
        if len(words) != 2:
            return [], f"clause {raw.strip()!r} is not 'ROLE ARG'"
        role, arg = words
        if role in ORDERED_ROLES:
            if not EDGE_RE.match(arg):
                return [], f"bad edge name {arg!r}"
        elif role == "relaxed":
            if arg not in RELAXED_REASONS:
                return (
                    [],
                    f"relaxed reason {arg!r} not one of {', '.join(RELAXED_REASONS)}",
                )
        elif role == "local":
            if arg not in LOCAL_REASONS:
                return [], f"local reason {arg!r} not one of {', '.join(LOCAL_REASONS)}"
        else:
            return [], f"unknown role {role!r}"
        clauses.append(Clause(role, arg))
    roles = {c.role for c in clauses}
    if len(clauses) > 1 and roles & {"relaxed", "local"}:
        return [], "relaxed and local stand alone"
    edges = [c.arg for c in clauses if c.role in ORDERED_ROLES]
    if len(edges) != len(set(edges)):
        return [], "an edge appears twice"
    return clauses, None


# -- per-file scan -----------------------------------------------------


def _call_args(tokens: list[Token], open_idx: int) -> tuple[list[str], int]:
    """Top-level argument texts of the call whose '(' is at open_idx,
    and the index of its ')'."""
    depth, args, cur = 0, [], []
    for k in range(open_idx, len(tokens)):
        t = tokens[k].text
        if t in "([{":
            depth += 1
            if depth == 1:
                continue
        elif t in ")]}":
            depth -= 1
            if depth == 0:
                if cur or args:
                    args.append(" ".join(cur))
                return args, k
        elif t == "," and depth == 1:
            args.append(" ".join(cur))
            cur = []
            continue
        cur.append(t)
    return args, len(tokens) - 1


def _check_orders(
    site: Site, kind: str, order_args: list[str]
) -> frozenset[str] | None:
    """The guarantees the operation provides, or None when its order
    cannot be judged (problems recorded on the site)."""
    if not order_args:
        return CAPABILITY[kind] & ORDERS["memory_order_seq_cst"]
    sets = []
    for a in order_args:
        if a not in ORDERS:
            site.problems.append(f"memory order {a!r} is not a constant")
            return None
        sets.append(ORDERS[a])
    if any("consume" in s for s in sets):
        site.problems.append(
            "memory_order_consume: compilers promote it to acquire; write acquire"
        )
        return None
    cap = CAPABILITY[kind]
    if kind == "load" and REL in sets[0] and SC not in sets[0]:
        site.problems.append("a load cannot be release or acq_rel")
        return None
    if kind == "store" and ACQ in sets[0] and SC not in sets[0]:
        site.problems.append("a store cannot be acquire or acq_rel")
        return None
    if kind == "cas":
        success, failure = sets
        # A failed CAS is a load.  (C17 dropped "failure no stronger than
        # success", as C++17 did, so release/acquire is a valid pair.)
        if REL in failure and SC not in failure:
            site.problems.append("CAS failure order cannot be release or acq_rel")
            return None
        return cap & (success | failure)
    return cap & sets[0]


def scan_text(path: str, src: str) -> tuple[list[Site], list[Tag], list[Violation]]:
    tokens, comments = lex(src)
    tags: list[Tag] = []
    violations: list[Violation] = []
    for c in comments:
        if not TAG_LOOSE_RE.search(c.text):
            continue
        clauses, err = parse_tag(c.text)
        tag = Tag(path, c.line, c.offset, c.text, clauses, err)
        tags.append(tag)
        if err:
            violations.append(Violation(path, c.line, f"bad tag: {err}"))

    calls: list[tuple[int, int, str]] = []  # (name index, close index, name)
    for k, t in enumerate(tokens):
        if t.text in OPS and k + 1 < len(tokens) and tokens[k + 1].text == "(":
            if k > 0 and tokens[k - 1].text == "define":
                continue  # a macro named like an operation
            _, close = _call_args(tokens, k + 1)
            calls.append((k, close, t.text))

    call_starts = {k for k, _, _ in calls}
    sites: list[Site] = []
    for k, close, name in calls:
        kind, idxs = OPS[name]
        args, _ = _call_args(tokens, k + 1)
        order_args = [args[i] if i < len(args) else "" for i in idxs]
        site = Site(path, tokens[k].line, name, order_args)
        provided = _check_orders(site, kind, order_args)

        # Leading tag: the nearest unused tag after the statement boundary.
        b = k - 1
        while b >= 0 and tokens[b].text not in (";", "{", "}") and b not in call_starts:
            b -= 1
        lo = tokens[b].offset if b >= 0 else -1
        hi = tokens[k].offset
        cand = [t for t in tags if lo < t.offset < hi and not t.used]
        if not cand:
            close_line = tokens[close].line
            nxt = min((s for s in call_starts if s > k), default=None)
            limit = tokens[nxt].offset if nxt is not None else len(src) + 1
            cand = [
                t
                for t in tags
                if t.offset > tokens[close].offset
                and t.line == close_line
                and t.offset < limit
                and not t.used
            ][:1]
        if cand:
            site.tag = cand[-1]
            site.tag.used = True
        sites.append(site)

        for p in site.problems:
            violations.append(Violation(path, site.line, f"{name}: {p}"))
        if site.tag is None:
            violations.append(Violation(path, site.line, f"{name}: no @sync tag"))
            continue
        if site.tag.error or provided is None:
            continue
        roles = {c.role for c in site.tag.clauses}
        if roles & {"local"}:
            continue
        claimed: frozenset[str] = frozenset()
        for c in site.tag.clauses:
            claimed |= ORDERED_ROLES.get(c.role, frozenset())
        claimed &= CAPABILITY[kind]
        missing = claimed - provided
        extra = provided - claimed
        if missing:
            violations.append(
                Violation(
                    path,
                    site.line,
                    f"{name}: UNDER-ORDERED: the tag claims {', '.join(sorted(missing))} "
                    f"but the order provides {_describe(provided)}",
                )
            )
        elif extra:
            violations.append(
                Violation(
                    path,
                    site.line,
                    f"{name}: OVER-ORDERED: the order provides {', '.join(sorted(extra))} "
                    f"that no tag clause justifies",
                )
            )

    for t in tags:
        if not t.used and not t.error:
            violations.append(
                Violation(path, t.line, "tag attached to no atomic operation")
            )
    return sites, tags, violations


def _describe(provided: frozenset[str]) -> str:
    return ", ".join(sorted(provided)) if provided else "nothing (relaxed)"


# -- tree --------------------------------------------------------------


def source_files(repo: Path) -> list[Path]:
    src = repo / "src"
    out = []
    for p in sorted(src.rglob("*")):
        if p.suffix not in (".c", ".h") or not p.is_file():
            continue
        rel = p.relative_to(src)
        if rel.parts and rel.parts[0] in EXCLUDE_DIRS:
            continue
        out.append(p)
    return out


def run_audit(repo: Path, paths: list[Path] | None = None) -> AuditResult:
    result = AuditResult()
    files = paths if paths is not None else source_files(repo)
    for p in files:
        rel = str(p.relative_to(repo)) if p.is_absolute() else str(p)
        text = (repo / rel).read_text(errors="replace")
        sites, tags, violations = scan_text(rel, text)
        result.sites += sites
        result.tags += tags
        result.violations += violations
    # With a file subset, an edge whose other side lives elsewhere reads
    # as one-sided: the subset check is a working aid, the gate is the
    # whole tree.
    result.violations += edge_violations(result)
    return result


def edge_violations(result: AuditResult) -> list[Violation]:
    out = []
    for edge, members in result.edges().items():
        roles = {r for _, r in members}
        first = members[0][0]
        publishes = roles & {"release", "acq_rel", "seq_cst"}
        observes = roles & {"acquire", "acq_rel", "seq_cst"}
        if not publishes or not observes:
            side = "publishing" if not publishes else "observing"
            out.append(
                Violation(first.path, first.line, f"edge {edge!r} has no {side} side")
            )
        if "seq_cst" in roles and roles != {"seq_cst"}:
            out.append(
                Violation(
                    first.path,
                    first.line,
                    f"edge {edge!r} mixes seq_cst with weaker roles: "
                    "the total-order argument needs seq_cst everywhere on it",
                )
            )
    return out


def summary_line(result: AuditResult) -> str:
    tagged = sum(1 for s in result.sites if s.tag is not None)
    return (
        f"atomicaudit: {len(result.sites)} atomic operations, {tagged} tagged, "
        f"{len(result.edges())} edges, {len(result.violations)} violations"
    )


def render_report(result: AuditResult) -> str:
    md = ["# Atomic ordering report", "", summary_line(result), ""]
    if result.violations:
        md += ["## Violations", ""]
        md += [f"- {v.path}:{v.line}: {v.message}" for v in result.violations]
        md.append("")
    md += ["## Edges (who publishes, who observes)", ""]
    for edge, members in result.edges().items():
        md.append(f"### {edge}")
        for s, role in members:
            md.append(f"- {role:8} {s.path}:{s.line} {s.op}")
        md.append("")
    relaxed: dict[str, list[Site]] = {}
    for s in result.sites:
        if s.tag and not s.tag.error:
            for c in s.tag.clauses:
                if c.role in ("relaxed", "local"):
                    relaxed.setdefault(f"{c.role} {c.arg}", []).append(s)
    if relaxed:
        md += ["## Unordered operations by reason", ""]
        for reason, sites in sorted(relaxed.items()):
            md.append(f"### {reason}")
            md += [f"- {s.path}:{s.line} {s.op}" for s in sites]
            md.append("")
    return "\n".join(md) + "\n"
