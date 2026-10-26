"""Allocation-ownership audit for datakit.

Every heap allocation site must declare its ownership discipline in one
of exactly two machine-verifiable ways:

  1. SCOPED — the declaration carries the platform scope-exit cleanup
     attribute (``cleaner(fn)`` / ``cleanerName(fn, Tag)`` from
     ``src/cleaner.h``): the allocation is freed on every exit path of
     the owning scope.

  2. ANNOTATED — the allocation intentionally outlives the function and
     carries a structured ownership comment, either trailing on the
     allocation line or alone on the line directly above it::

         /* @own: caller */
         /* @own: caller; free=multimapFree */
         /* @own: container; free=flexFree */
         /* @own: static */
         /* @own: cache */

     Grammar (validated; malformed annotations are violations)::

         @own: (caller|container|static|cache) (; free=IDENT)?

     caller    — returned or written through an out-param; the caller
                 must release it (``free=`` names the releasing call).
     container — adopted into a struct; the struct's Free releases it.
     static    — deliberate process-lifetime allocation, never freed.
     cache     — owned by a cache/pool that manages reuse and release.

A site that is neither SCOPED nor validly ANNOTATED is a violation.
``check`` mode exits nonzero unless the tree is 100% clean; ``report``
mode additionally classifies each site's apparent data flow so the
migration can proceed file by file with evidence in hand.

Site discovery is lexical over clang's token stream (pre-expansion), so
allocator calls inside macro definitions are audited at the definition
site, and matches inside comments or strings are impossible.  Data-flow
classification is heuristic and clearly labeled as such in reports.
"""

from __future__ import annotations

import re
import time
from dataclasses import dataclass, field
from pathlib import Path

from clang import cindex

from . import procs

ALLOCATORS = ("zmalloc", "zcalloc", "zrealloc")

#: Container constructors: every call site must carry the
#: same SCOPED/ANNOTATED discipline as a raw allocation. Maps each
#: constructor to its releasing function (used for report suggestions
#: and to seed `free=` annotations). Names are post-expansion C
#: identifiers. Single source of truth — extend HERE only.
CONSTRUCTORS = {
    "zmemdup": "zfree",
    "flexNew": "flexFree",
    "flexDuplicate": "flexFree",
    "mflexNew": "mflexFree",
    "mflexDuplicate": "mflexFree",
    "mflexStateNew": "mflexStateFree",
    "mflexStateCreate": "mflexStateFree",
    "multimapNew": "multimapFree",
    "multimapNewLimit": "multimapFree",
    "multimapCopy": "multimapFree",
    "multimapSmallNew": "multimapSmallFree",
    "multimapSmallCopy": "multimapSmallFree",
    "multimapMediumNew": "multimapMediumFree",
    "multimapMediumCopy": "multimapMediumFree",
    "multimapFullNew": "multimapFullFree",
    "multimapFullSetNew": "multimapFullFree",
    "multimapFullCopy": "multimapFullFree",
    "multilistNew": "multilistFree",
    "multilistNewLimit": "multilistFree",
    "multilistCopy": "multilistFree",
    "multidictNew": "multidictFree",
    "multidictDefaultClassNew": "multidictDefaultClassFree",
    "multiarrayNew": "multiarrayFree",
    "intsetNew": "intsetFree",
    "intsetBigNew": "intsetBigFree",
    "intsetU32New": "intsetU32Free",
    "multiroarBitNew": "multiroarFree",
    "multiroarValueNew": "multiroarFree",
    "multiroarDuplicate": "multiroarFree",
    "multiroarFromArray": "multiroarFree",
    "multiroarDeserialize": "multiroarFree",
    "multiroarNewAnd": "multiroarFree",
    "multiroarNewOr": "multiroarFree",
    "multiroarNewXor": "multiroarFree",
    "multiroarNewNot": "multiroarFree",
    "multiroarNewAndNot": "multiroarFree",
    "multiroarNewAndN": "multiroarFree",
    "multiroarNewOrN": "multiroarFree",
    "multiroarNewXorN": "multiroarFree",
    "multiOrderedSetNew": "multiOrderedSetFree",
    "multilruNew": "multilruFree",
    "multilruNewWithConfig": "multilruFree",
    "stringPoolNew": "stringPoolFree",
    "atomPoolNew": "atomPoolFree",
    "atomPoolNewDefault": "atomPoolFree",
    "timerWheelNew": "timerWheelFree",
    "multiTimerNew": "multiTimerFree",
    "multiFenwickNew": "multiFenwickFree",
    "multiFenwickNewFromArray": "multiFenwickFree",
    "multiFenwickFlatNew": "multiFenwickFlatFree",
    "fenwick2dI16New": "fenwick2dI16Free",
    "fenwick2dI32New": "fenwick2dI32Free",
    "fenwick2dI64New": "fenwick2dI64Free",
    "fenwick2dI128New": "fenwick2dI128Free",
    "fenwick2dU16New": "fenwick2dU16Free",
    "fenwick2dU32New": "fenwick2dU32Free",
    "fenwick2dU64New": "fenwick2dU64Free",
    "fenwick2dU128New": "fenwick2dU128Free",
    "fenwick2dFloatNew": "fenwick2dFloatFree",
    "fenwick2dDoubleNew": "fenwick2dDoubleFree",
    "multiFenwick2dNew": "multiFenwick2dFree",
    "multiFenwickFlatNewFromArray": "multiFenwickFlatFree",
    "aeadNonceLogCreate": "aeadNonceLogFree",
    "clusterRingNew": "clusterRingFree",
    "clusterRingNewDefault": "clusterRingFree",
    "listCreate": "listRelease",
    "hyperloglogNew": "hyperloglogFree",
    "mdsnew": "mdsfree",
    "mdsnewlen": "mdsfree",
    "mdsempty": "mdsfree",
    "mdsdup": "mdsfree",
    "mdscnew": "mdscfree",
    "mdscnewlen": "mdscfree",
    "mdscempty": "mdscfree",
    "mdscdup": "mdscfree",
    "mdsemptylen": "mdsfree",
    "mdsNewFromFile": "mdsfree",
    "mdscemptylen": "mdscfree",
    "mdscNewFromFile": "mdscfree",
    # every other constructor whose result the caller owns ([[nodiscard]]
    # in its header), by family
    "multimapNewCompress": "multimapFree",
    "multimapNewConfigure": "multimapFree",
    "multimapSetNew": "multimapFree",
    "multimapMediumNewFromOneGrow": "multimapMediumFree",
    "multilistNewFromFlex": "multilistFree",
    "multidictCopy": "multidictFree",
    "multiarrayCopy": "multiarrayFree",
    "multiarrayNewFromArray": "multiarrayFree",
    "intsetCopy": "intsetFree",
    "intsetBigCopy": "intsetBigFree",
    "intsetBigNewFromMembers": "intsetBigFree",
    "intsetU32Copy": "intsetU32Free",
    "intsetU32NewLen": "intsetU32Free",
    "multiroarDeserializeRaw": "multiroarFree",
    "multiOrderedSetNewLimit": "multiOrderedSetFree",
    "multiOrderedSetNewCompress": "multiOrderedSetFree",
    "multiOrderedSetCopy": "multiOrderedSetFree",
    "multiOrderedSetUnion": "multiOrderedSetFree",
    "multiOrderedSetIntersect": "multiOrderedSetFree",
    "multiOrderedSetDifference": "multiOrderedSetFree",
    "multilruNewWithLevels": "multilruFree",
    "multilruNewWithLevelsCapacity": "multilruFree",
    "stringPoolNewWithIds": "stringPoolFree",
    "atomPoolNewWithIds": "atomPoolFree",
    "fenwick2dI16NewFromArray": "fenwick2dI16Free",
    "fenwick2dI32NewFromArray": "fenwick2dI32Free",
    "fenwick2dI64NewFromArray": "fenwick2dI64Free",
    "fenwick2dI128NewFromArray": "fenwick2dI128Free",
    "fenwick2dU16NewFromArray": "fenwick2dU16Free",
    "fenwick2dU32NewFromArray": "fenwick2dU32Free",
    "fenwick2dU64NewFromArray": "fenwick2dU64Free",
    "fenwick2dU128NewFromArray": "fenwick2dU128Free",
    "fenwick2dFloatNewFromArray": "fenwick2dFloatFree",
    "fenwick2dDoubleNewFromArray": "fenwick2dDoubleFree",
    "aeadNonceLogFromBytes": "aeadNonceLogFree",
    "clusterRingCopy": "clusterRingFree",
    "clusterRingDeserialize": "clusterRingFree",
    "listDup": "listRelease",
    "hyperloglogNewSparse": "hyperloglogFree",
    "hyperloglogNewDense": "hyperloglogFree",
    "hyperloglogCopy": "hyperloglogFree",
    "hyperloglogNewUnion": "hyperloglogFree",
    "hyperloglogNewFromRaw": "hyperloglogFree",
}

#: Directories under src/ that are vendored and out of audit scope.
EXCLUDE_DIRS = ("memshard",)

#: ``#define zmalloc(...)`` in datakit.h defines the allocators, and
#: seam macros like ``#define IMATH_CALLOC(...) zcalloc(...)`` re-point
#: another library's allocator at ours; any function-like ``#define``
#: whose NAME contains alloc/ALLOC is an allocator *definition*, not an
#: allocation site.
ALLOC_DEFINE_RE = re.compile(r"^\s*#\s*define\s+\w*(?:alloc|ALLOC)\w*\s*\(")

ANNOT_RE = re.compile(
    r"@own:\s*(?P<cls>caller|container|static|cache)"
    r"(?:\s*;\s*free=(?P<free>[A-Za-z_][A-Za-z0-9_]*))?\s*(?:\*/)?\s*\\?\s*$"
)

#: Any @own-looking text that fails the strict grammar is itself a
#: violation (a rotted or typo'd annotation must never read as clean).
ANNOT_LOOSE_RE = re.compile(r"@own\b")

CLEANUP_MARKERS = ("cleaner", "cleanerName", "cleanup")


@dataclass
class Site:
    path: str
    line: int
    col: int
    allocator: str
    function: str
    statement: str
    status: str  # SCOPED | ANNOTATED | VIOLATION
    annotation: str | None = None
    flow: str = "unclassified"  # heuristic data-flow class
    evidence: list[str] = field(default_factory=list)
    early_return_risk: int = 0


@dataclass
class AuditResult:
    sites: list[Site] = field(default_factory=list)
    parse_errors: list[str] = field(default_factory=list)

    @property
    def scoped(self) -> list[Site]:
        return [s for s in self.sites if s.status == "SCOPED"]

    @property
    def annotated(self) -> list[Site]:
        return [s for s in self.sites if s.status == "ANNOTATED"]

    @property
    def violations(self) -> list[Site]:
        return [s for s in self.sites if s.status == "VIOLATION"]

    @property
    def clean(self) -> bool:
        return not self.violations and not self.parse_errors


@dataclass(frozen=True, slots=True)
class _Loc:
    line: int
    column: int


@dataclass(frozen=True, slots=True)
class _Tok:
    """A libclang token's spelling, kind and location, read once.

    Every ``cindex.Token`` attribute is a libclang call that builds a
    new object, and the classification reads each token's location
    once per allocation site in its function: on the large files that
    made the audit quadratic in libclang calls (minutes per file on a
    loaded machine)."""

    spelling: str
    kind: cindex.TokenKind
    location: _Loc


def _compile_args(repo: Path, path: Path) -> list[str]:
    """Compile args for a TU: from compile_commands.json when present,
    else a minimal include set sufficient for tokenization."""
    fallback = [
        "-std=gnu11",
        f"-I{repo / 'src'}",
        f"-I{repo / 'deps'}",
        f"-I{repo / 'deps' / 'varint' / 'src'}",
    ]
    if path.suffix == ".h":
        # Headers have no compile command; parse them as plain C so
        # clang does not reject the translation unit outright.
        return ["-x", "c", *fallback]
    ccdb_dir = repo / "build"
    if not (ccdb_dir / "compile_commands.json").exists():
        return fallback
    try:
        db = cindex.CompilationDatabase.fromDirectory(str(ccdb_dir))
        cmds = db.getCompileCommands(str(path))
        if not cmds:
            return fallback
        args = list(cmds[0].arguments)[1:]  # drop compiler argv[0]
        # Drop the source path and -o/-c plumbing; keep -I/-D/-std.
        keep: list[str] = []
        skip_next = False
        for a in args:
            if skip_next:
                skip_next = False
                continue
            if a in ("-o", "-c"):
                skip_next = a == "-o"
                continue
            if a.endswith((".c", ".o")):
                continue
            keep.append(a)
        return keep
    except cindex.CompilationDatabaseError:
        return fallback


def _function_extents(
    tu: cindex.TranslationUnit, path: str
) -> list[tuple[int, int, str]]:
    """(startLine, endLine, name) for every function defined in path.

    Only the translation unit's top-level cursors are visited: C has
    no nested function definitions (clang rejects GNU nested
    functions), and walking every statement of every included header
    dominated the audit's run time."""
    out = []
    for c in tu.cursor.get_children():
        if (
            c.kind == cindex.CursorKind.FUNCTION_DECL
            and c.is_definition()
            and c.location.file
            and c.location.file.name == path
        ):
            out.append((c.extent.start.line, c.extent.end.line, c.spelling))
    return out


def _containing_function(extents: list[tuple[int, int, str]], line: int) -> str:
    best = ""
    best_span = 1 << 60
    for start, end, name in extents:
        if start <= line <= end and (end - start) < best_span:
            best, best_span = name, end - start
    return best or "<toplevel>"


def _statement_span(tokens: list, idx: int) -> tuple[int, int]:
    """Token index range [lo, hi] of the statement containing tokens[idx].
    An initializer brace (`= { ... }`) is part of the statement; only
    block braces terminate the backward walk."""
    lo = idx
    while lo > 0:
        prev = tokens[lo - 1].spelling
        if prev in (";", "}"):
            break
        if prev == "{":
            if lo - 2 >= 0 and tokens[lo - 2].spelling == "=":
                lo -= 1
                continue
            break
        lo -= 1
    hi = idx
    while hi + 1 < len(tokens) and tokens[hi].spelling != ";":
        hi += 1
    return lo, hi


def _annotation_for(
    lines: list[str], line: int, stmt_line: int | None = None
) -> tuple[str | None, bool]:
    """(annotation text, malformed?) from the site line, the line above,
    or — when the statement wraps so the allocator token sits on a
    continuation line — the statement's first line and the line above
    that."""
    check_lines = [line]
    if stmt_line is not None and stmt_line != line:
        check_lines.append(stmt_line)
    candidates = []
    for ln in check_lines:
        if ln - 1 < len(lines):
            candidates.append(lines[ln - 1])
        if ln - 2 >= 0:
            candidates.append(lines[ln - 2])
    for text in candidates:
        if ANNOT_LOOSE_RE.search(text):
            m = ANNOT_RE.search(text)
            if m:
                ann = m.group("cls")
                if m.group("free"):
                    ann += f"; free={m.group('free')}"
                return ann, False
            return text.strip(), True
    return None, False


def _scoped_vars(fn_tokens: list) -> set[str]:
    """Variable names declared with a cleaner()/cleanerName() attribute
    anywhere in the function (the platform idiom often declares
    ``cleaner(zfree) void *x = NULL;`` and allocates on a later line)."""
    out: set[str] = set()
    spelled = [t.spelling for t in fn_tokens]
    for i, s in enumerate(spelled):
        if s not in CLEANUP_MARKERS:
            continue
        # Require a call shape — `cleaner(`, `cleanerName(`, or a raw
        # attribute's `cleanup(` — so `goto cleanup;` and `cleanup:`
        # labels never match.
        if i + 1 >= len(spelled) or spelled[i + 1] != "(":
            continue
        # Declared identifier: the last identifier before '=' or ';'
        # within this declaration statement.
        declared = None
        for j in range(i + 1, min(i + 40, len(spelled))):
            nxt = spelled[j]
            if nxt in ("=", ";"):
                break
            if nxt.isidentifier() and nxt not in (
                "void",
                "const",
                "struct",
                "unsigned",
                "signed",
            ):
                declared = nxt
        if declared:
            out.add(declared)
    return out


def _classify_flow(
    tokens: list,
    lo: int,
    hi: int,
    site_idx: int,
    fn_tokens: list,
    scoped_vars: set[str],
    site: Site,
) -> None:
    """Heuristic data-flow classification; fills site.flow/evidence/risk."""
    stmt = [t.spelling for t in tokens[lo : hi + 1]]
    site.statement = " ".join(stmt)[:160]

    if any(m in stmt for m in CLEANUP_MARKERS):
        site.flow = "scoped"
        return

    if stmt and stmt[0] == "return":
        site.flow = "transfer:return"
        return

    # Find the '=' that feeds the allocator call, then inspect its LHS.
    eq = -1
    for i in range(lo, site_idx):
        if tokens[i].spelling == "=" and tokens[i + 1].spelling != "=":
            eq = i
    if eq < 0:
        site.flow = "expression"  # allocator used directly as an argument
        return

    lhs = [t.spelling for t in tokens[lo:eq]]
    if "[" in lhs and lhs and lhs[0].isidentifier() and lhs[0] in scoped_vars:
        # `scopedArray[i] = zmalloc(...)` — the array itself carries a
        # cleanup attribute that releases every element.
        site.flow = "scoped"
        return
    if "->" in lhs or "." in lhs:
        site.flow = "transfer:store"
        return
    if lhs and lhs[0] == "*":
        site.flow = "transfer:outparam"
        return

    # Plain local variable: follow its uses through the rest of the
    # containing function.
    var = lhs[-1] if lhs else None
    if not var or not var.isidentifier():
        site.flow = "unclassified"
        return

    if var in scoped_vars:
        # Declared earlier with a cleaner attribute; this assignment
        # fills a scope-managed variable.
        site.flow = "scoped"
        return

    freed_at = None
    escaped = None
    returns_before_free = 0
    after = [
        t
        for t in fn_tokens
        if t.location.line > tokens[hi].location.line
        or (
            t.location.line == tokens[hi].location.line
            and t.location.column > tokens[hi].location.column
        )
    ]
    spelled = [t.spelling for t in after]
    for i, s in enumerate(spelled):
        if s == "return" and freed_at is None and escaped is None:
            nxt = spelled[i + 1] if i + 1 < len(spelled) else ""
            if nxt == var:
                escaped = f"returned at line {after[i].location.line}"
                break
            returns_before_free += 1
        if s == var:
            prev = spelled[i - 1] if i > 0 else ""
            prev2 = spelled[i - 2] if i > 1 else ""
            nxt = spelled[i + 1] if i + 1 < len(spelled) else ""
            if prev == "(" and (
                prev2 in ("zfree", "free")
                or prev2.endswith(("Free", "free", "Release"))
            ):
                freed_at = f"{prev2} at line {after[i].location.line}"
                break
            if prev == "=" and prev2 in scoped_vars:
                # `scopedVar = var;` — the allocation merges into a
                # scope-managed variable (the zrealloc-of-a-scoped-
                # buffer idiom); ownership stays with the cleaner.
                site.flow = "scoped"
                site.evidence.append(
                    f"merged into scoped '{prev2}' at line {after[i].location.line}"
                )
                return
            if prev == "=" and (
                "->" in spelled[max(0, i - 6) : i]
                or spelled[max(0, i - 3) : i - 1][-1:] == ["*"]
            ):
                escaped = f"stored at line {after[i].location.line}"
                break

    if escaped:
        site.flow = "transfer:viaLocal"
        site.evidence.append(escaped)
    elif freed_at:
        site.flow = "local:freed"
        site.evidence.append(freed_at)
        site.early_return_risk = returns_before_free
    else:
        site.flow = "local:unresolved"


def audit_file(
    repo: Path, path: Path, index: cindex.Index, include_constructors: bool = False
) -> AuditResult:
    result = AuditResult()
    try:
        source = path.read_text(errors="replace")
    except OSError as e:
        result.parse_errors.append(f"{path}: {e}")
        return result
    lines = source.splitlines()

    args = _compile_args(repo, path)
    try:
        tu = index.parse(
            str(path),
            args=args,
            options=cindex.TranslationUnit.PARSE_DETAILED_PROCESSING_RECORD,
        )
    except cindex.TranslationUnitLoadError as e:
        result.parse_errors.append(f"{path}: {e}")
        return result

    extents = _function_extents(tu, str(path))
    # Comments are excluded so statement spans anchor on real code (a
    # leading comment line would otherwise become the statement start
    # and shift where annotations are looked up).
    tokens = []
    for t in tu.cursor.get_tokens():
        loc = t.location
        if loc.file and loc.file.name == str(path):
            kind = t.kind
            if kind != cindex.TokenKind.COMMENT:
                tokens.append(_Tok(t.spelling, kind, _Loc(loc.line, loc.column)))

    for i, tok in enumerate(tokens):
        if tok.kind != cindex.TokenKind.IDENTIFIER:
            continue
        is_ctor = include_constructors and tok.spelling in CONSTRUCTORS
        if tok.spelling not in ALLOCATORS and not is_ctor:
            continue
        if i + 1 >= len(tokens) or tokens[i + 1].spelling != "(":
            continue
        if is_ctor and i > 0 and tokens[i - 1].spelling == "*":
            # `flex *flexNew(void)` — the constructor's own
            # declaration/definition (every constructor returns a
            # pointer), not a call site.
            continue
        line = tok.location.line
        if line - 1 < len(lines) and ALLOC_DEFINE_RE.match(lines[line - 1]):
            continue  # the allocator's own #define, not a use

        site = Site(
            path=str(path.relative_to(repo)),
            line=line,
            col=tok.location.column,
            allocator=tok.spelling,
            function=_containing_function(extents, line),
            statement="",
            status="VIOLATION",
        )

        lo, hi = _statement_span(tokens, i)
        fn_span = next(((s, e) for s, e, _ in extents if s <= line <= e), None)
        fn_tokens = (
            [
                t
                for t in tokens
                if fn_span and fn_span[0] <= t.location.line <= fn_span[1]
            ]
            if fn_span
            else []
        )
        if not fn_tokens:
            # Template headers and macro bodies parse without function
            # cursors; fall back to whole-file tokens so cleaner
            # declarations and later frees are still visible (an
            # over-approximation, acceptable for classification).
            fn_tokens = tokens
        _classify_flow(tokens, lo, hi, i, fn_tokens, _scoped_vars(fn_tokens), site)
        if is_ctor:
            site.evidence.append(f"pair: {CONSTRUCTORS[tok.spelling]}")

        annotation, malformed = _annotation_for(
            lines, line, stmt_line=tokens[lo].location.line
        )
        if site.flow == "scoped":
            site.status = "SCOPED"
        elif annotation and not malformed:
            site.status = "ANNOTATED"
            site.annotation = annotation
        elif annotation and malformed:
            site.status = "VIOLATION"
            site.annotation = f"MALFORMED: {annotation}"

        result.sites.append(site)

    return result


def discover(repo: Path) -> list[Path]:
    src = repo / "src"
    out = []
    for p in sorted(src.rglob("*.c")) + sorted(src.rglob("*.h")):
        rel = p.relative_to(src)
        if any(part in EXCLUDE_DIRS for part in rel.parts):
            continue
        out.append(p)
    return out


#: Seconds the audit waits for some file to finish before it fails:
#: the slowest file takes about 3 s alone, so only a stuck worker
#: reaches this even on a heavily loaded machine.  Below the
#: allocOwnership ctest TIMEOUT (300 s), so a stuck run fails with
#: this module's message rather than being killed by ctest.
STALL_TIMEOUT = 120.0

#: Per-worker libclang Index (libclang contexts must not be shared
#: across processes); created by the pool initializer in each worker.
_WORKER_INDEX: cindex.Index | None = None


def _init_worker() -> None:
    """Pool initializer: load libclang in the worker itself."""
    global _WORKER_INDEX
    _WORKER_INDEX = cindex.Index.create()


@dataclass
class FileAudit:
    """One worker's answer: the file's result and the seconds it took."""

    result: AuditResult
    seconds: float


def _audit_one(args: tuple[str, str, bool]) -> FileAudit:
    """Process-pool map fn: audit a single file with the worker's Index."""
    repo_s, path_s, ctors = args
    if _WORKER_INDEX is None:
        _init_worker()
    assert _WORKER_INDEX is not None
    start = time.monotonic()
    result = audit_file(
        Path(repo_s), Path(path_s), _WORKER_INDEX, include_constructors=ctors
    )
    return FileAudit(result, time.monotonic() - start)


def _size(path: Path) -> int:
    try:
        return path.stat().st_size
    except OSError:
        return 0


def run_audit(
    repo: Path,
    jobs: int | None = None,
    include_constructors: bool = False,
    stall_timeout: float | None = STALL_TIMEOUT,
    timings: list[tuple[str, float]] | None = None,
) -> AuditResult:
    """Map: parse+audit each file in parallel worker processes
    (``procs.parallel_map``: spawn-started workers, a dead worker or a
    stall past `stall_timeout` seconds is a ToolError, never a hang).
    Reduce: merge per-file results in stable file order.

    Files are handed out largest first so the slowest ones start early
    instead of running alone at the end.  When `timings` is given it
    receives (repo-relative path, seconds) for every file."""
    files = discover(repo)
    workers = procs.sizing(jobs).build
    order = sorted(range(len(files)), key=lambda i: -_size(files[i]))
    work = [(str(repo), str(files[i]), include_constructors) for i in order]
    answers = procs.parallel_map(
        _audit_one,
        work,
        jobs=workers,
        stall_timeout=stall_timeout,
        init=_init_worker,
        describe=lambda item: str(Path(item[1]).relative_to(repo)),
        noun="file",
    )
    by_file: list[FileAudit | None] = [None] * len(files)
    for i, answer in zip(order, answers):
        by_file[i] = answer
    total = AuditResult()
    for path, answer in zip(files, by_file):
        assert answer is not None
        total.sites.extend(answer.result.sites)
        total.parse_errors.extend(answer.result.parse_errors)
        if timings is not None:
            timings.append((str(path.relative_to(repo)), answer.seconds))
    return total


def summary_line(r: AuditResult) -> str:
    return (
        f"ALLOC OWNERSHIP: {len(r.sites)} sites — "
        f"{len(r.scoped)} scoped, {len(r.annotated)} annotated, "
        f"{len(r.violations)} unclassified"
        + (f", {len(r.parse_errors)} parse errors" if r.parse_errors else "")
    )


def render_report(r: AuditResult) -> str:
    flows: dict[str, int] = {}
    for s in r.sites:
        flows[s.flow] = flows.get(s.flow, 0) + 1

    lines = [
        "# Allocation Ownership Audit",
        "",
        f"`{summary_line(r)}`",
        "",
        "Target end-state: every site SCOPED (cleaner attribute) or",
        "ANNOTATED (`@own:` grammar — see tools/src/dktools/allocaudit.py).",
        "",
        "## Data-flow classification (heuristic)",
        "",
        "| flow | sites | meaning |",
        "|---|---|---|",
    ]
    meaning = {
        "scoped": "cleaner attribute present — already clean",
        "transfer:return": "allocation is returned — annotate `caller`",
        "transfer:store": "stored into a struct field — annotate `container`",
        "transfer:outparam": "written through out-param — annotate `caller`",
        "transfer:viaLocal": "local that escapes later — annotate per dest",
        "local:freed": "freed in-function — CONVERT TO cleaner()",
        "local:unresolved": "no free/escape found — investigate first",
        "expression": "allocator used inline in an expression",
        "unclassified": "statement shape not recognized",
    }
    for flow in sorted(flows, key=flows.get, reverse=True):
        lines.append(f"| `{flow}` | {flows[flow]} | {meaning.get(flow, '')} |")

    risky = sorted(
        (s for s in r.sites if s.early_return_risk), key=lambda s: -s.early_return_risk
    )
    if risky:
        lines += [
            "",
            "## Leak-risk hotspots (returns between alloc and free)",
            "",
            "| site | function | returns before free |",
            "|---|---|---|",
        ]
        for s in risky[:40]:
            lines.append(
                f"| {s.path}:{s.line} | `{s.function}` | {s.early_return_risk} |"
            )

    lines += ["", "## Sites by file", ""]
    by_file: dict[str, list[Site]] = {}
    for s in r.sites:
        by_file.setdefault(s.path, []).append(s)
    for path in sorted(by_file):
        lines.append(f"### {path}")
        lines.append("")
        lines.append("| line | fn | alloc | status | flow | note |")
        lines.append("|---|---|---|---|---|---|")
        for s in by_file[path]:
            note = s.annotation or "; ".join(s.evidence)
            lines.append(
                f"| {s.line} | `{s.function}` | {s.allocator} "
                f"| {s.status} | `{s.flow}` | {note} |"
            )
        lines.append("")

    if r.parse_errors:
        lines += ["## Parse errors", ""]
        lines += [f"- {e}" for e in r.parse_errors]

    return "\n".join(lines) + "\n"
