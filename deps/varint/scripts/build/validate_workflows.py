#!/usr/bin/env python3
"""Validate every GitHub Actions workflow file parses as YAML.

Catches indentation/quoting mistakes locally before a push burns a CI
run discovering them. Exits non-zero naming the first broken file.

Usage: scripts/build/validate_workflows.py
"""

import pathlib
import sys

import yaml

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKFLOW_DIR = REPO_ROOT / ".github" / "workflows"


def main() -> int:
    files = sorted(WORKFLOW_DIR.glob("*.yml")) + sorted(
        WORKFLOW_DIR.glob("*.yaml")
    )
    if not files:
        print(f"no workflow files found under {WORKFLOW_DIR}", file=sys.stderr)
        return 1
    for path in files:
        try:
            doc = yaml.safe_load(path.read_text())
        except yaml.YAMLError as exc:
            print(f"FAIL {path.relative_to(REPO_ROOT)}: {exc}", file=sys.stderr)
            return 1
        if not isinstance(doc, dict) or "jobs" not in doc:
            print(
                f"FAIL {path.relative_to(REPO_ROOT)}: parsed but has no jobs:",
                file=sys.stderr,
            )
            return 1
        print(f"OK   {path.relative_to(REPO_ROOT)} ({len(doc['jobs'])} jobs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
