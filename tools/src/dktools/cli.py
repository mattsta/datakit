"""dktools — datakit platform automation CLI."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import allocaudit, perfdiff, toolchain, verify
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
        help="build HEAD plus exactly the given paths in isolation and test it "
        "(exit 0 pass, 1 fail, 2 input error)",
    )
    vf.add_argument("name", help="work-dir name under ROOT/verify")
    vf.add_argument(
        "specs",
        nargs="+",
        metavar="PATH[=FILE]",
        help="repo path to take from the working tree, or FILE's content "
        "staged at PATH",
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

    args = parser.parse_args(argv)

    try:
        return _dispatch(args)
    except ToolError as exc:
        print(f"dktools {args.command}: {exc}", file=sys.stderr)
        return 2


def _dispatch(args: argparse.Namespace) -> int:
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
            ),
        )

    if args.command == "perfdiff":
        return perfdiff.run(args.baseline, args.current, args.tolerance)

    if args.command == "allocaudit":
        result = allocaudit.run_audit(
            args.repo, jobs=args.jobs, include_constructors=args.constructors
        )
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
