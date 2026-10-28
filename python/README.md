# datakit for Python

`datakit.persist` reads datakit's persisted files (snapshots, WALs and quarantine records) for operators:
inspect headers, verify every envelope, walk a WAL and see where and why it stops, summarize what recovery
rebuilds, and read quarantined bytes. Every call opens files read-only, so nothing it does changes a file.

The package is a thin layer over a C extension; every decode and check is datakit's own. One wheel per
platform (stable ABI `abi3t`) loads on Python 3.15+, regular and free-threaded.

Operators use the commands (`datakit`, or `python -m datakit`): `inspect`, `verify`, `dump`, `stats` and
`quarantine`, with `--json` for machine-readable output. The guide is
[docs/modules/persist/OPERATORS.md](../docs/modules/persist/OPERATORS.md).

Build and test from the repository root:

```
uv run --project tools dktools pybuild NAME
uv run --project tools dktools pytest NAME
```
