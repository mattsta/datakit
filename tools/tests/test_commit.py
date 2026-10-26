"""``dktools commit``: commit exactly what a passing verify built.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import unittest

from dktools import commit
from dktools.gitutil import ToolError
from repofixture import IsolatedGitEnv, TempRepo

MESSAGE = (
    "a: change\n\nWhat: a changes.\n\nWhy: a test.\n\nHow: by hand.\n\n"
    "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\n"
)


class Commits(unittest.TestCase):
    def setUp(self) -> None:
        self._env = IsolatedGitEnv().__enter__()
        self.repo = TempRepo()
        self.repo.write("src/a.c", "int a;\n")
        self.repo.write("src/b.c", "int b;\n")
        self.repo.write("src/gone.c", "int gone;\n")
        self.repo.commit("base", "src/a.c", "src/b.c", "src/gone.c")
        self.root = self.repo.path.parent / "scratch"
        self.message = self.repo.path.parent / "msg.txt"
        self.message.write_text(MESSAGE)

    def tearDown(self) -> None:
        self.repo.cleanup()
        self._env.__exit__()

    def verified(self, result: str = "PASS", **files: str | None) -> None:
        """A verify work dir 't' whose tree holds 'files' (None: deleted)."""
        work = self.root / "verify" / "t"
        (work / "src" / "src").mkdir(parents=True, exist_ok=True)
        (work / "result.txt").write_text(f"verify t: {result}\n  tree ...\n")
        for name, text in files.items():
            path = work / "src" / "src" / name.replace("_", ".")
            if text is None:
                path.unlink(missing_ok=True)
            else:
                path.write_text(text)

    def commit(self, *paths: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = commit.run(self.repo.path, "t", self.message, list(paths), self.root)
        return code, out.getvalue()

    def head_has(self, rel: str) -> str:
        return self.repo.git("show", f"HEAD:{rel}")

    def test_commits_exactly_the_named_paths_as_verified(self) -> None:
        self.repo.write("src/a.c", "int a2;\n")
        self.repo.write("src/b.c", "int b2;\n")  # changed, not named: stays out
        self.verified(a_c="int a2;\n", b_c="int b2;\n")
        code, out = self.commit("src/a.c")
        self.assertEqual(code, 0, out)
        self.assertEqual(self.head_has("src/a.c"), "int a2;")
        self.assertEqual(self.head_has("src/b.c"), "int b;")
        self.assertEqual(self.repo.git("log", "-1", "--format=%s"), "a: change")
        self.assertEqual(self.repo.git("diff", "--name-only"), "src/b.c")
        self.assertEqual(self.repo.git("diff", "--cached", "--name-only"), "")

    def test_a_deletion_commits_as_verified(self) -> None:
        (self.repo.path / "src" / "gone.c").unlink()
        self.verified(gone_c=None)
        code, out = self.commit("src/gone.c")
        self.assertEqual(code, 0, out)
        self.assertNotIn("src/gone.c", self.repo.git("ls-files"))

    def test_refuses_a_verify_that_did_not_pass(self) -> None:
        self.repo.write("src/a.c", "int a2;\n")
        self.verified("FAIL", a_c="int a2;\n")
        code, out = self.commit("src/a.c")
        self.assertEqual(code, 1)
        self.assertIn("did not pass", out)
        self.assertEqual(self.head_has("src/a.c"), "int a;")

    def test_refuses_a_path_that_differs_from_the_verified_tree(self) -> None:
        self.repo.write("src/a.c", "int a3;\n")  # edited after the verify
        self.verified(a_c="int a2;\n")
        code, out = self.commit("src/a.c")
        self.assertEqual(code, 1)
        self.assertIn("src/a.c differs from what verify t built", out)
        self.assertEqual(self.repo.git("diff", "--cached", "--name-only"), "")

    def test_refuses_when_the_index_holds_other_changes(self) -> None:
        self.repo.write("src/a.c", "int a2;\n")
        self.repo.write("src/b.c", "int b2;\n")
        self.repo.git("add", "src/b.c")
        self.verified(a_c="int a2;\n")
        code, out = self.commit("src/a.c")
        self.assertEqual(code, 1)
        self.assertIn("already staged: src/b.c", out)

    def test_refuses_a_message_without_what_why_how(self) -> None:
        self.repo.write("src/a.c", "int a2;\n")
        self.verified(a_c="int a2;\n")
        self.message.write_text("a: change\n\nWhat: only this.\n")
        with self.assertRaisesRegex(ToolError, "Why:, How:"):
            self.commit("src/a.c")

    def test_a_hard_leak_unstages_and_commits_nothing(self) -> None:
        text = "/* " + "trip" + "wire */ int a2;\n"
        self.repo.write("src/a.c", text)
        self.verified(a_c=text)
        code, out = self.commit("src/a.c")
        self.assertEqual(code, 1)
        self.assertIn("LEAK", out)
        self.assertEqual(self.head_has("src/a.c"), "int a;")
        self.assertEqual(self.repo.git("diff", "--cached", "--name-only"), "")

    def test_needs_paths_and_a_verify_result(self) -> None:
        with self.assertRaises(ToolError):
            self.commit()
        with self.assertRaisesRegex(ToolError, "no verify result"):
            self.commit("src/a.c")


if __name__ == "__main__":
    unittest.main()
