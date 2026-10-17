"""dktools — datakit platform automation CLI."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from . import allocaudit


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

    args = parser.parse_args(argv)

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
