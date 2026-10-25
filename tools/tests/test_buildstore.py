"""``dktools builds`` and the managed build directories.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

from dktools import buildstore, cli

GB = 1 << 30


def dead_pid() -> int:
    """A process id that has exited."""
    proc = subprocess.Popen([sys.executable, "-c", "pass"])
    proc.wait()
    return proc.pid


class Repo:
    """A fake repository: presets, a build root, a scratch root."""

    def __init__(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        (self.root / "CMakePresets.json").write_text(
            json.dumps(
                {
                    "configurePresets": [
                        {"name": "_base", "hidden": True},
                        {"name": "dev"},
                        {"name": "perf"},
                    ]
                }
            )
        )
        self.scratch = buildstore.default_scratch(self.root)
        # the real old scratch root stays out of every test
        self._saved = os.environ.get(buildstore.ENV_LEGACY)
        os.environ[buildstore.ENV_LEGACY] = str(self.root / "no-old-scratch")

    def close(self) -> None:
        if self._saved is None:
            os.environ.pop(buildstore.ENV_LEGACY, None)
        else:
            os.environ[buildstore.ENV_LEGACY] = self._saved
        self._tmp.cleanup()

    def work(self, kind: str, name: str, size: int, age: float) -> Path:
        """An idle managed build of `size` bytes last used `age` s ago."""
        path = self.scratch / kind / name
        lease = buildstore.claim(path, kind, name, "abc", kind)
        (path / "blob").write_bytes(b"x" * size)
        lease.release()
        data = json.loads((path / buildstore.MANIFEST).read_text())
        data["lastUsed"] = time.time() - age
        (path / buildstore.MANIFEST).write_text(json.dumps(data))
        return path


class Claims(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = Repo()

    def tearDown(self) -> None:
        self.repo.close()

    def test_a_claim_holds_its_lock_until_released(self) -> None:
        path = self.repo.scratch / "verify" / "one"
        lease = buildstore.claim(path, "verify", "one", "abc", "verify")
        self.assertEqual(buildstore.lock_holder(path), os.getpid())
        (path / "file").write_bytes(b"12345")
        lease.release()
        self.assertIsNone(buildstore.lock_holder(path))
        data = json.loads((path / buildstore.MANIFEST).read_text())
        self.assertEqual(data["kind"], "verify")
        self.assertEqual(data["revision"], "abc")
        self.assertGreaterEqual(data["size"], 5)

    def test_a_dead_owners_lock_is_idle(self) -> None:
        path = self.repo.scratch / "verify" / "crashed"
        buildstore.claim(path, "verify", "crashed")
        (path / buildstore.LOCK).write_text(f"{dead_pid()}\n")
        self.assertIsNone(buildstore.lock_holder(path))
        (found,) = buildstore.inventory(self.repo.root, self.repo.scratch)
        self.assertFalse(found.in_use)


class Inventory(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = Repo()

    def tearDown(self) -> None:
        self.repo.close()

    def test_kinds(self) -> None:
        root = self.repo.root
        self.repo.work("verify", "v", 10, 0)
        (root / "build" / "dev").mkdir(parents=True)
        (root / "build" / "CMakeCache.txt").write_text("old single build")
        (root / "build-laneA").mkdir()
        kinds = {
            (b.kind, b.name) for b in buildstore.inventory(root, self.repo.scratch)
        }
        self.assertIn(("verify", "v"), kinds)
        self.assertIn(("preset", "dev"), kinds)
        self.assertIn(("legacy", "build-laneA"), kinds)
        self.assertIn(("legacy", "build/CMakeCache.txt"), kinds)
        self.assertNotIn(("preset", "perf"), kinds)  # not built


class Collection(unittest.TestCase):
    def setUp(self) -> None:
        self.repo = Repo()

    def tearDown(self) -> None:
        self.repo.close()

    def policy(self, budget: int, age_days: float = 14.0) -> buildstore.Policy:
        return buildstore.Policy(budget, age_days * 86400.0)

    def test_past_age_then_least_recent_until_under_budget(self) -> None:
        blob = 100_000  # the manifests' few hundred bytes do not matter
        old = self.repo.work("verify", "old", blob, 20 * 86400)
        a = self.repo.work("perfcompare", "a", blob, 300)
        b = self.repo.work("perfcompare", "b", blob, 200)
        c = self.repo.work("validate", "c", blob, 100)
        found = buildstore.inventory(self.repo.root, self.repo.scratch)
        removed = buildstore.collect(found, self.policy(int(2.5 * blob)))
        gone = {r.path for r in removed}
        self.assertEqual(gone, {old, a})
        self.assertFalse(old.exists() or a.exists())
        self.assertTrue(b.exists() and c.exists())

    def test_never_a_build_in_use_presets_and_legacy_only_when_asked(self) -> None:
        root = self.repo.root
        busy = self.repo.scratch / "verify" / "busy"
        buildstore.claim(busy, "verify", "busy")  # held by this process
        data = json.loads((busy / buildstore.MANIFEST).read_text())
        data["lastUsed"] = time.time() - 99 * 86400
        (busy / buildstore.MANIFEST).write_text(json.dumps(data))
        (root / "build" / "dev").mkdir(parents=True)
        (root / "build-old").mkdir()
        found = buildstore.inventory(root, self.repo.scratch)
        self.assertEqual(buildstore.collect(found, self.policy(0)), [])
        self.assertTrue(busy.exists())
        policy = self.policy(0)
        policy.presets = policy.legacy = True
        found = buildstore.inventory(root, self.repo.scratch)
        removed = {r.kind for r in buildstore.collect(found, policy, dry_run=True)}
        self.assertEqual(removed, {"preset", "legacy"})
        self.assertTrue((root / "build" / "dev").exists())  # dry run

    def test_fresh_build_collects_the_others_not_itself(self) -> None:
        os.environ[buildstore.ENV_BUDGET] = "0"
        try:
            other = self.repo.work("verify", "other", 100, 50)
            target = self.repo.work("verify", "me", 100, 9 * 86400)
            with contextlib.redirect_stderr(io.StringIO()):
                path = buildstore.fresh_build(
                    self.repo.root, self.repo.scratch, "verify", "me"
                )
            self.assertEqual(path, target)
            self.assertFalse(other.exists())
            # emptied, then claimed again
            self.assertEqual(
                sorted(p.name for p in path.iterdir()),
                sorted([buildstore.MANIFEST, buildstore.LOCK]),
            )
        finally:
            del os.environ[buildstore.ENV_BUDGET]


class Command(unittest.TestCase):
    def test_list_and_dry_run_gc(self) -> None:
        repo = Repo()
        try:
            repo.work("verify", "v", 10, 30 * 86400)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(
                    cli.main(["--repo", str(repo.root), "builds", "list"]), 0
                )
            self.assertIn("verify", out.getvalue())
            self.assertIn("1 builds", out.getvalue())
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                code = cli.main(["--repo", str(repo.root), "builds", "gc", "--dry-run"])
            self.assertEqual(code, 0)
            self.assertIn("would remove 1 builds", out.getvalue())
            self.assertTrue((repo.scratch / "verify" / "v").exists())
        finally:
            repo.close()


if __name__ == "__main__":
    unittest.main()
