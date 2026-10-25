"""``dktools historyscrub``: edits from a commit, the check, the rewrite.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import os
import subprocess
import unittest

from dktools import buildstore, cli, historyscrub
from dktools.gitutil import ToolError
from repofixture import GIT_ENV, IsolatedGitEnv, TempRepo

BLOCK = " *\n * Copyright (c) 2024, Someone\n * All rights reserved.\n"
TOP = "/* a.h - the a header\n"
END = " */\n"
FORBID = [r"All rights reserved"]


def header(body: str, block: str = BLOCK) -> str:
    return TOP + block + END + body


class ScrubRepo(unittest.TestCase):
    """main: add (block) -> change body -> remove block -> later commit.
    A tag and a side branch sit on the second commit; other.c holds a
    notice of its own that no edit names."""

    def setUp(self) -> None:
        self._env = IsolatedGitEnv().__enter__()
        self._legacy = os.environ.get(buildstore.ENV_LEGACY)
        self.repo = TempRepo()
        os.environ[buildstore.ENV_LEGACY] = str(self.repo.path / "no-old-scratch")
        r = self.repo
        r.write("src/a.h", header("int a;\n"))
        r.write(
            "src/other.c",
            "/* Copyright (c) 2009, Upstream\n * All rights reserved.\n */\n",
        )
        self.add = r.commit("add a", "src/a.h", "src/other.c")
        r.write("src/a.h", header("int a2;\n"))
        self.body = r.commit("a: body", "src/a.h")
        r.git("tag", "v1")
        r.git("branch", "side")
        r.write("src/a.h", TOP + END + "int a2;\n")
        self.remove = r.commit("a: remove the block", "src/a.h")
        r.write("src/b.c", "int b;\n")
        self.later = r.commit(f"b: added (a came in {self.add[:10]})", "src/b.c")
        self.scratch = r.path.parent / "scratch"

    def tearDown(self) -> None:
        if self._legacy is None:
            os.environ.pop(buildstore.ENV_LEGACY, None)
        else:
            os.environ[buildstore.ENV_LEGACY] = self._legacy
        self.repo.cleanup()
        self._env.__exit__()

    def edits(self) -> list[historyscrub.Edit]:
        return historyscrub.edits_from_commit(self.repo.path, self.remove)

    def show(self, rev: str, path: str) -> str:
        return self.repo.git("show", f"{rev}:{path}")


class Edits(ScrubRepo):
    def test_a_commits_hunks_are_the_edits(self) -> None:
        (edit,) = self.edits()
        self.assertEqual(edit.path, "src/a.h")
        self.assertEqual(edit.old, BLOCK.encode())
        self.assertEqual(edit.new, b"")

    def test_only_removals_and_replacements_of_modified_files(self) -> None:
        with self.assertRaisesRegex(ToolError, "only modified files"):
            historyscrub.edits_from_commit(self.repo.path, self.later)  # adds b.c
        self.repo.write("src/a.h", TOP + END + "int a2;\nint more;\n")
        added = self.repo.commit("a: one more line", "src/a.h")
        with self.assertRaisesRegex(ToolError, "only adds lines"):
            historyscrub.edits_from_commit(self.repo.path, added)


class Check(ScrubRepo):
    def test_clean_check_counts_versions(self) -> None:
        found = historyscrub.scan(self.repo.path, self.edits(), FORBID)
        self.assertTrue(found.clean, found.lines())
        (a,) = found.paths
        self.assertEqual((a.versions, a.changed, a.remaining), (3, 2, []))

    def test_a_variant_the_edits_miss_is_reported(self) -> None:
        self.repo.git("checkout", "-q", "side")
        self.repo.write("src/a.h", header("int s;\n", BLOCK.replace("2024", "2023")))
        self.repo.commit("side: variant", "src/a.h")
        self.repo.git("checkout", "-q", "main")
        found = historyscrub.scan(self.repo.path, self.edits(), FORBID)
        self.assertFalse(found.clean)
        self.assertEqual(len(found.paths[0].remaining), 1)
        with self.assertRaisesRegex(ToolError, "check is not clean"):
            historyscrub.rewrite(
                self.repo.path, self.scratch, "t", self.edits(), FORBID
            )

    def test_a_copy_in_another_file_is_reported(self) -> None:
        self.repo.write("src/copy.h", TOP + BLOCK + END)
        self.repo.commit("copy", "src/copy.h")
        found = historyscrub.scan(self.repo.path, self.edits(), FORBID)
        self.assertEqual(found.copies, {"src/a.h": ["src/copy.h"]})
        self.assertFalse(found.clean)


class Rewrite(ScrubRepo):
    def test_every_ref_loses_the_text_and_nothing_else_changes(self) -> None:
        head_tree = self.repo.git("rev-parse", "HEAD^{tree}")
        other = self.show("HEAD", "src/other.c")
        done = historyscrub.rewrite(
            self.repo.path, self.scratch, "t", self.edits(), FORBID
        )
        r = self.repo
        self.assertEqual(done.dropped, [self.remove])
        self.assertEqual(r.git("rev-parse", "HEAD^{tree}"), head_tree)
        self.assertEqual(r.git("rev-list", "--count", "HEAD"), "3")
        for ref in ("main", "side", "v1"):
            self.assertNotIn("All rights reserved", self.show(ref, "src/a.h"))
        self.assertEqual(self.show("v1", "src/a.h"), TOP + END + "int a2;")
        self.assertEqual(self.show("HEAD", "src/other.c"), other)
        # a hash quoted in a later message follows its commit
        (new_add,) = (
            old_new[1] for old_new in historyscrub.translate(done.work, [self.add])
        )
        self.assertIn(new_add[:10], r.git("log", "-1", "--format=%s", "HEAD"))
        # the old objects are gone; the bundle keeps them
        gone = subprocess.run(
            ["git", "-C", str(r.path), "cat-file", "-e", self.add],
            env={**os.environ, **GIT_ENV},
        )
        self.assertNotEqual(gone.returncode, 0)
        r.git("bundle", "verify", str(done.bundle))
        self.assertFalse((r.path / ".git" / "filter-repo").exists())
        # the same edits, rebuilt by hand: the commit that held them is gone
        after = [historyscrub.Edit("src/a.h", BLOCK.encode(), b"")]
        self.assertTrue(historyscrub.scan(r.path, after, FORBID).clean)

    def test_refuses_changed_tracked_files(self) -> None:
        self.repo.write("src/b.c", "int changed;\n")
        with self.assertRaisesRegex(ToolError, "tracked files have changes"):
            historyscrub.rewrite(
                self.repo.path, self.scratch, "t", self.edits(), FORBID
            )


class Command(ScrubRepo):
    def dktools(self, *argv: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = cli.main(
                [
                    "--repo",
                    str(self.repo.path),
                    "historyscrub",
                    *argv,
                    "--root",
                    str(self.scratch),
                ]
            )
        return code, out.getvalue()

    def test_check_rewrite_map(self) -> None:
        code, out = self.dktools(
            "check", "--from-commit", self.remove, "--forbid", FORBID[0]
        )
        self.assertEqual(code, 0, out)
        self.assertIn(
            "src/a.h: 3 versions, 2 changed by the edits, 0 still forbidden", out
        )
        code, out = self.dktools(
            "rewrite", "--from-commit", self.remove, "--forbid", FORBID[0]
        )
        self.assertEqual(code, 0, out)
        self.assertIn(f"dropped {self.remove[:10]}", out)
        self.assertIn("historyscrub rewrite: ok", out)
        code, out = self.dktools("map", self.body[:8])
        self.assertEqual(code, 0)
        self.assertEqual(out.split()[0], self.body[:8])
        self.assertEqual(len(out.split()[1]), 40)
        self.assertEqual(self.dktools("check")[0], 2)  # needs --from-commit


if __name__ == "__main__":
    unittest.main()
