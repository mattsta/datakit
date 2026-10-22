"""dktools — datakit platform automation CLI."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import (
    allocaudit,
    leakcheck,
    perfdiff,
    procs,
    testtime,
    toolchain,
    validate,
    vendorsync,
    verify,
)
from .gitutil import ToolError


def _repo_default() -> Path:
    """The datakit repo root: nearest ancestor holding src/datakit.h."""
    here = Path.cwd()
    for candidate in (here, *here.parents):
        if (candidate / "src" / "datakit.h").exists():
            return candidate
    # tool checkout location is repo/tools/src/dktools
    return Path(__file__).resolve().parents[3]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="dktools")
    parser.add_argument(
        "--repo",
        type=Path,
        default=_repo_default(),
        help="datakit repo root (auto-detected)",
    )
    sub = parser.add_subparsers(dest="command", required=True)

    aa = sub.add_parser(
        "allocaudit", help="allocation-ownership audit (scoped/annotated/violations)"
    )
    aa.add_argument(
        "mode",
        choices=("report", "check"),
        help="report: write full markdown audit; "
        "check: exit nonzero unless 100%% clean",
    )
    aa.add_argument(
        "--out",
        type=Path,
        default=None,
        help="report output path (default: stdout)",
    )
    aa.add_argument(
        "-j",
        "--jobs",
        type=int,
        default=None,
        help="parallel worker processes (default: cpu count)",
    )
    aa.add_argument(
        "--constructors",
        action="store_true",
        help="also audit container-constructor call sites "
        "(flexNew/multimapNew/... — see CONSTRUCTORS)",
    )
    aa.add_argument(
        "--timeout",
        type=float,
        default=allocaudit.STALL_TIMEOUT,
        metavar="SECONDS",
        help="fail (exit 2) when no file finishes for this long "
        f"(default {allocaudit.STALL_TIMEOUT:g}; 0 waits forever)",
    )
    aa.add_argument(
        "--timings",
        type=int,
        default=0,
        metavar="N",
        help="print the N slowest files and their seconds to stderr",
    )

    tt = sub.add_parser(
        "testtime",
        help="time each section of one datakit test (exit 0 pass, 1 fail)",
    )
    tt.add_argument("binary", type=Path, help="the datakit-test binary")
    tt.add_argument("test", help="test name, as for 'datakit-test test NAME'")
    tt.add_argument(
        "--top", type=int, default=15, help="sections to show (default 15)"
    )
    tt.add_argument(
        "--timeout",
        type=float,
        default=1800.0,
        help="stop the test after this many seconds (default 1800)",
    )
    tt.add_argument(
        "--prefix",
        action="append",
        default=[],
        help="another section header prefix (repeatable), for suites that "
        "print their own, such as 'Test '",
    )

    pd = sub.add_parser(
        "perfdiff",
        help="compare two perfRegression TSV outputs "
        "(exit 0 clean, 1 regression, 2 input error)",
    )
    pd.add_argument("baseline", type=Path, help="baseline TSV path")
    pd.add_argument("current", type=Path, help="current TSV path")
    pd.add_argument(
        "--tolerance",
        type=float,
        default=10.0,
        help="regression tolerance percent (default 10.0)",
    )

    vf = sub.add_parser(
        "verify",
        help="build HEAD plus exactly the given paths, the index or every "
        "changed tracked path in isolation and test it "
        "(exit 0 pass, 1 fail, 2 input error)",
    )
    vf.add_argument("name", help="work-dir name under ROOT/verify")
    vf.add_argument(
        "specs",
        nargs="*",
        metavar="PATH[=FILE]",
        help="repo path to take from the working tree, or FILE's content "
        "staged at PATH (not with --staged or --all-changed)",
    )
    vf_source = vf.add_mutually_exclusive_group()
    vf_source.add_argument(
        "--staged",
        action="store_true",
        help="build exactly the current index (HEAD plus what is staged)",
    )
    vf_source.add_argument(
        "--all-changed",
        action="store_true",
        help="build HEAD plus every tracked path that differs in the working "
        "tree (modified, added or deleted)",
    )
    vf.add_argument("--tests", default=None, help="ctest name regex, run in parallel")
    vf.add_argument(
        "--serial",
        default=None,
        help="ctest name regex, run one at a time (excluded from --tests)",
    )
    vf.add_argument(
        "--compiler",
        choices=toolchain.COMPILERS,
        default=toolchain.DEFAULT_COMPILER,
        help=f"named compiler (default {toolchain.DEFAULT_COMPILER})",
    )
    vf.add_argument(
        "--preset",
        choices=tuple(toolchain.PRESETS),
        default=None,
        help="build preset (default: the compiler's first; gcc15 builds release)",
    )
    vf.add_argument(
        "--target",
        action="append",
        default=None,
        help="build target, repeatable (default: the four preset targets)",
    )
    vf.add_argument(
        "--root",
        type=Path,
        default=None,
        help="scratch root (env DKTOOLS_SCRATCH, default: dktools/ in the temp dir)",
    )
    vf.add_argument("-j", "--jobs", type=int, default=None, help="parallel jobs")
    toolchain.add_arguments(vf)

    lc = sub.add_parser(
        "leakcheck",
        help="leakage gate over the staged diff, a range, or the tree "
        "(exit 0 clean, 1 hard hit, 2 input error)",
    )
    lc_mode = lc.add_mutually_exclusive_group()
    lc_mode.add_argument(
        "--staged",
        action="store_const",
        dest="mode",
        const="staged",
        help="added lines of the staged diff (default)",
    )
    lc_mode.add_argument(
        "--range",
        dest="rev_range",
        metavar="A..B",
        default=None,
        help="added lines of git diff A..B (A...B: from the merge base)",
    )
    lc_mode.add_argument(
        "--tree",
        action="store_const",
        dest="mode",
        const="tree",
        help="every tracked file under src/ docs/ include/ tools/ and top-level *.md",
    )
    lc.add_argument(
        "--include-vendored",
        action="store_true",
        help="also check deps/ and src/memshard/",
    )

    va = sub.add_parser(
        "validate",
        help="full compiler x preset matrix on an exported revision "
        "(exit 0 pass, 1 fail, 2 input error)",
    )
    va.add_argument("--rev", default="HEAD", help="revision to export (default HEAD)")
    va.add_argument(
        "--compilers",
        default=",".join(toolchain.COMPILERS),
        help=f"comma-separated compilers (default {','.join(toolchain.COMPILERS)})",
    )
    va.add_argument(
        "--presets",
        default="dev,debug",
        help="comma-separated presets (default dev,debug; gcc15 builds release)",
    )
    va.add_argument(
        "--serial",
        default=validate.DEFAULT_SERIAL,
        help="ctest name regex run one at a time (default: the timing-sensitive tests)",
    )
    va.add_argument(
        "--skip",
        default=validate.DEFAULT_SKIP,
        help=f"ctest name regex never run (default {validate.DEFAULT_SKIP})",
    )
    va.add_argument(
        "--debug-skip",
        default=validate.DEFAULT_DEBUG_SKIP,
        help="ctest name regex also skipped in debug builds "
        f"(default {validate.DEFAULT_DEBUG_SKIP}; '' runs them)",
    )
    va.add_argument(
        "--tsan-skip",
        default=validate.DEFAULT_TSAN_SKIP,
        help="ctest name regex also skipped in tsan builds "
        f"(default {validate.DEFAULT_TSAN_SKIP}; '' runs them)",
    )
    va.add_argument(
        "--leak-base",
        default=validate.DEFAULT_LEAK_BASE,
        help="leakage gate checks merge-base(BASE, REV)..REV "
        f"(default {validate.DEFAULT_LEAK_BASE}; '' skips it)",
    )
    va.add_argument(
        "--root",
        type=Path,
        default=None,
        help="scratch root (env DKTOOLS_SCRATCH, default: dktools/ in the temp dir)",
    )
    va.add_argument("-j", "--jobs", type=int, default=None, help="parallel jobs")
    toolchain.add_arguments(va)

    vs = sub.add_parser(
        "vendorsync",
        help="mirror a vendored dependency from its upstream repository "
        "(exit 0 synced, 1 refused, 2 input error)",
    )
    vs.add_argument(
        "name", help=f"vendored dependency ({', '.join(vendorsync.VENDORED)})"
    )
    vs.add_argument(
        "--upstream", type=Path, required=True, help="upstream git checkout"
    )
    vs.add_argument("--rev", default="HEAD", help="upstream revision (default HEAD)")
    vs.add_argument(
        "--stage",
        action="store_true",
        help="stage exactly the paths whose index entry differs from upstream",
    )
    vs.add_argument(
        "--accept-local-loss",
        action="store_true",
        help="overwrite datakit versions that never existed upstream",
    )
    vs.add_argument(
        "--force-ignored",
        action="store_true",
        help="with --stage, git add -f the upstream files .gitignore hides",
    )
    vs.add_argument(
        "--dry-run", action="store_true", help="report only; write and stage nothing"
    )

    args = parser.parse_args(argv)

    # Children run in their own process groups (procs), so a terminal
    # Ctrl-C reaches only dktools: KeyboardInterrupt and these handlers
    # unwind through the finally blocks that stop them.
    try:
        with procs.signal_handlers():
            return _dispatch(args)
    except ToolError as exc:
        print(f"dktools {args.command}: {exc}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print(f"dktools {args.command}: interrupted", file=sys.stderr)
        return 130


def _split(value: str) -> list[str]:
    return [part.strip() for part in value.split(",") if part.strip()]


def _dispatch(args: argparse.Namespace) -> int:
    if args.command == "vendorsync":
        return vendorsync.run(
            args.repo,
            args.name,
            args.upstream,
            rev=args.rev,
            do_stage=args.stage,
            accept_local_loss=args.accept_local_loss,
            force_ignored=args.force_ignored,
            dry_run=args.dry_run,
        )

    if args.command == "validate":
        return validate.run(
            args.repo,
            validate.ValidateOptions(
                rev=args.rev,
                compilers=[
                    toolchain.from_args(n, args) for n in _split(args.compilers)
                ],
                presets=_split(args.presets),
                serial=args.serial,
                skip=args.skip,
                debug_skip=args.debug_skip,
                tsan_skip=args.tsan_skip,
                leak_base=args.leak_base,
                root=args.root,
                jobs=args.jobs,
            ),
        )

    if args.command == "leakcheck":
        mode = "range" if args.rev_range is not None else args.mode or "staged"
        return leakcheck.run(args.repo, mode, args.rev_range, args.include_vendored)

    if args.command == "verify":
        return verify.run(
            args.repo,
            verify.VerifyOptions(
                name=args.name,
                specs=args.specs,
                tests=args.tests,
                serial=args.serial,
                compiler=toolchain.from_args(args.compiler, args),
                preset=args.preset,
                targets=args.target,
                root=args.root,
                jobs=args.jobs,
                staged=args.staged,
                all_changed=args.all_changed,
            ),
        )

    if args.command == "perfdiff":
        return perfdiff.run(args.baseline, args.current, args.tolerance)

    if args.command == "testtime":
        if args.top < 1 or args.timeout <= 0:
            raise ToolError("--top must be at least 1 and --timeout positive")
        return testtime.run(
            args.binary, args.test, args.timeout, args.top, tuple(args.prefix)
        )

    if args.command == "allocaudit":
        if args.jobs is not None and args.jobs < 1:
            raise ToolError(f"-j must be at least 1 (got {args.jobs})")
        if args.timeout < 0:
            raise ToolError(f"--timeout must not be negative (got {args.timeout:g})")
        timings: list[tuple[str, float]] = []
        result = allocaudit.run_audit(
            args.repo,
            jobs=args.jobs,
            include_constructors=args.constructors,
            stall_timeout=args.timeout or None,
            timings=timings,
        )
        if args.timings > 0:
            for path, seconds in sorted(timings, key=lambda t: -t[1])[: args.timings]:
                print(f"{seconds:8.2f}s  {path}", file=sys.stderr)
        if args.mode == "report":
            report = allocaudit.render_report(result)
            if args.out is None:
                # The report owns stdout; the summary goes to stderr.
                print(allocaudit.summary_line(result), file=sys.stderr)
                sys.stdout.write(report)
                return 0
            print(allocaudit.summary_line(result))
            args.out.parent.mkdir(parents=True, exist_ok=True)
            args.out.write_text(report)
            print(f"report: {args.out}")
            return 0
        print(allocaudit.summary_line(result))
        # check mode: the CI gate
        if result.clean:
            print("100% clean pattern")
            return 0
        for s in result.violations[:20]:
            print(
                f"  VIOLATION {s.path}:{s.line} in {s.function} "
                f"[{s.flow}] {s.annotation or ''}"
            )
        if len(result.violations) > 20:
            print(f"  ... and {len(result.violations) - 20} more")
        return 1

    return 2


if __name__ == "__main__":
    sys.exit(main())
