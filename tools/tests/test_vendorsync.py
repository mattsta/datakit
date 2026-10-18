"""``dktools vendorsync``: exact mirroring, idempotence, local-edit
refusal, ignored files and explicit staging, on throwaway repositories.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import os
import unittest
from pathlib import Path

from dktools import cli, vendorsync
from dktools.gitutil import ToolError

from repofixture import IsolatedGitEnv, TempRepo

DEST = vendorsync.VENDORED["varint"]


class VendorSync(unittest.TestCase):
    def setUp(self) -> None:
        self._env = IsolatedGitEnv().__enter__()
        # Upstream: v1, then v2 changes a file, flips an exec bit,
        # deletes one file and adds two (one hidden by datakit's
        # .gitignore).
        self.up = TempRepo()
        self.up.write("a.txt", "a1\n")
        self.up.write("run.sh", "echo\n", mode=0o644)
        self.up.write("old.txt", "old\n")
        self.v1 = self.up.commit("v1", "a.txt", "run.sh", "old.txt")
        self.up.write("a.txt", "a2\n")
        (self.up.path / "run.sh").chmod(0o755)
        (self.up.path / "old.txt").unlink()
        self.up.write("src/new.c", "int n;\n")
        self.up.write("scripts/build/x.sh", "make\n")
        self.v2 = self.up.commit(
            "v2: change things",
            "a.txt",
            "run.sh",
            "old.txt",
            "src/new.c",
            "scripts/build/x.sh",
        )

        # Datakit vendors v1.
        self.dk = TempRepo()
        self.dk.write(".gitignore", "build*\n")
        self.dk.write(f"{DEST}/a.txt", "a1\n")
        self.dk.write(f"{DEST}/run.sh", "echo\n", mode=0o644)
        self.dk.write(f"{DEST}/old.txt", "old\n")
        self.dk.commit(
            "vendor v1",
            ".gitignore",
            f"{DEST}/a.txt",
            f"{DEST}/run.sh",
            f"{DEST}/old.txt",
        )

    def tearDown(self) -> None:
        self.up.cleanup()
        self.dk.cleanup()
        self._env.__exit__()

    def sync(self, *args: str, rev: str | None = None) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        argv = [
            "--repo",
            str(self.dk.path),
            "vendorsync",
            "varint",
            "--upstream",
            str(self.up.path),
        ]
        argv += ["--rev", rev or self.v2, *args]
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cli.main(argv)
        return code, out.getvalue(), err.getvalue()

    def vendored(self, rel: str) -> Path:
        return self.dk.path / DEST / rel

    def test_mirror_then_idempotent(self) -> None:
        code, out, _ = self.sync("--dry-run")
        self.assertEqual(code, 0, out)
        self.assertIn("working tree (would write): 2 added, 2 modified, 1 deleted", out)
        self.assertEqual(self.vendored("a.txt").read_text(), "a1\n")

        code, out, _ = self.sync()
        self.assertEqual(code, 0, out)
        self.assertIn(f"upstream {self.v2[:7]} 'v2: change things'", out)
        self.assertIn("working tree (written): 2 added, 2 modified, 1 deleted", out)
        self.assertIn(f"A {DEST}/src/new.c", out)
        self.assertIn(f"D {DEST}/old.txt", out)
        self.assertEqual(self.vendored("a.txt").read_text(), "a2\n")
        self.assertTrue(os.access(self.vendored("run.sh"), os.X_OK))
        self.assertFalse(self.vendored("old.txt").exists())
        self.assertEqual(self.vendored("scripts/build/x.sh").read_text(), "make\n")
        self.assertIn("ignored: 1 upstream file(s) hidden by .gitignore", out)
        self.assertIn(f"{DEST}/scripts/build/x.sh", out)
        # Nothing was staged.
        self.assertEqual(self.dk.git("diff", "--cached", "--name-only"), "")
        self.assertIn(
            "vs index (not staged; --stage stages them): 2 added, 2 modified, 1 deleted",
            out,
        )

        # Again at the same rev: no further changes, ignored still listed.
        code, out, _ = self.sync()
        self.assertEqual(code, 0, out)
        self.assertIn("working tree (written): 0 added, 0 modified, 0 deleted", out)
        self.assertIn("ignored: 1", out)

    def test_stage_exact_paths(self) -> None:
        code, out, _ = self.sync("--stage")
        self.assertEqual(code, 0, out)
        staged = self.dk.git("diff", "--cached", "--name-status").splitlines()
        self.assertEqual(
            sorted(staged),
            sorted(
                [
                    f"M\t{DEST}/a.txt",
                    f"D\t{DEST}/old.txt",
                    f"M\t{DEST}/run.sh",
                    f"A\t{DEST}/src/new.c",
                ]
            ),
        )
        code, out, _ = self.sync("--stage", "--force-ignored")
        self.assertEqual(code, 0, out)
        self.assertIn(
            f"A\t{DEST}/scripts/build/x.sh",
            self.dk.git("diff", "--cached", "--name-status"),
        )
        code, out, _ = self.sync("--stage")
        self.assertIn(
            "vs index (nothing to stage): 0 added, 0 modified, 0 deleted", out
        )

    def test_local_edits_refuse(self) -> None:
        self.dk.write(f"{DEST}/a.txt", "a1 patched in datakit\n")
        self.dk.write(f"{DEST}/local.txt", "datakit only\n")
        self.dk.commit("local edits", f"{DEST}/a.txt", f"{DEST}/local.txt")

        code, out, _ = self.sync()
        self.assertEqual(code, 1, out)
        self.assertIn("LOCAL EDITS", out)
        self.assertIn(f"HEAD: {DEST}/a.txt", out)
        self.assertIn(f"HEAD: {DEST}/local.txt", out)
        self.assertIn("+a1 patched in datakit", out)
        self.assertIn("-a2", out)
        self.assertIn("refusing to sync", out)
        # Nothing written.
        self.assertEqual(self.vendored("a.txt").read_text(), "a1 patched in datakit\n")
        self.assertFalse(self.vendored("src/new.c").exists())

        code, out, _ = self.sync("--accept-local-loss")
        self.assertEqual(code, 0, out)
        self.assertEqual(self.vendored("a.txt").read_text(), "a2\n")
        self.assertFalse(self.vendored("local.txt").exists())

    def test_older_upstream_versions_are_not_local(self) -> None:
        # Datakit's v1 blobs exist in upstream history: no refusal, and
        # syncing back to v1 after staging v2 is also clean.
        code, out, _ = self.sync("--stage")
        self.assertEqual(code, 0, out)
        self.assertNotIn("LOCAL EDITS", out)
        code, out, _ = self.sync(rev=self.v1)
        self.assertEqual(code, 0, out)
        self.assertNotIn("LOCAL EDITS", out)
        self.assertEqual(self.vendored("a.txt").read_text(), "a1\n")
        self.assertEqual(self.vendored("old.txt").read_text(), "old\n")
        # new.c is tracked (staged) and not in v1: deleted, with its
        # now-empty directory.
        self.assertFalse(self.vendored("src").exists())

    def test_untracked_extras_are_left_alone(self) -> None:
        # A file the sync wrote but nobody staged is not tracked, so a
        # sync to a rev without it reports it and leaves it in place.
        self.sync()
        code, out, _ = self.sync(rev=self.v1)
        self.assertEqual(code, 0, out)
        self.assertTrue(self.vendored("src/new.c").exists())
        self.assertIn(
            f"untracked extras (not upstream, left alone): 1\n    {DEST}/src/new.c", out
        )

    def test_uncommitted_worktree_edit_is_local(self) -> None:
        self.vendored("a.txt").write_text("edited, never committed\n")
        code, out, _ = self.sync()
        self.assertEqual(code, 1)
        self.assertIn(f"working tree: {DEST}/a.txt", out)

    def test_input_errors(self) -> None:
        for args, rev in (((), "no-such-rev"), (("--force-ignored",), None)):
            with self.subTest(args=args):
                code, out, err = self.sync(*args, rev=rev)
                self.assertEqual(code, 2)
                self.assertEqual(len(err.strip().splitlines()), 1)
                self.assertNotIn("Traceback", err)
        with self.assertRaises(ToolError):
            vendorsync.run(self.dk.path, "nosuch", self.up.path)
        with self.assertRaises(ToolError):
            vendorsync.run(self.dk.path, "varint", self.up.path / "absent")


if __name__ == "__main__":
    unittest.main()
