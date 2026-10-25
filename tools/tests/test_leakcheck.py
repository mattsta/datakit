"""``dktools leakcheck``: the term list, the buckets and the three modes.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import unittest

from dktools import cli, leakcheck
from dktools.gitutil import ToolError
from repofixture import IsolatedGitEnv, TempRepo


def labels(text: str) -> list[tuple[str, bool]]:
    return [(h.term.label, h.term.hard) for h in leakcheck.scan("f", [(1, text)])]


class Terms(unittest.TestCase):
    def test_multi_word_terms_take_any_separator(self) -> None:
        for text in ("cross shard", "cross-shard", "crossShard", "CROSS_SHARD"):
            with self.subTest(text=text):
                self.assertEqual(labels(text), [("cross shard", True)])
        self.assertEqual(labels("the exit-wait pin"), [("exit-wait pin", True)])
        self.assertEqual(labels("release flavor"), [("release flavour", True)])

    def test_hard_terms(self) -> None:
        for text, label in (
            ("a Carrier build", "carrier"),
            ("see notes/plans/x.md", "notes/"),
            ("(notes/designs)", "notes/"),
            ("INV-A holds", "INV-X"),
            ("log 20261017_120000.txt", "timestamp"),
            ("open /tmp/x", "/tmp/"),
            ("vertical 3 of", "vertical N"),
            ("design § 4", "design §"),
        ):
            with self.subTest(text=text):
                self.assertIn((label, True), labels(text))

    def test_review_terms_and_case(self) -> None:
        self.assertEqual(labels("a P1 finding"), [("P<digit>", False)])
        self.assertEqual(labels("int p1 = 0;"), [])
        self.assertEqual(labels("ARM B wins"), [("ARM X", False)])
        self.assertEqual(labels("raise an arm b"), [])
        self.assertEqual(labels("arm 2 of the trial"), [("arm N", False)])
        self.assertEqual(labels("ship the product"), [("the product", False)])

    def test_quoted_output_allows_only_its_term_in_its_file(self) -> None:
        note = "warning: (arm64) /tmp/lto.o unable to open object file"
        quoted = "tools/src/dktools/steps.py"
        self.assertEqual(leakcheck.scan(quoted, [(1, note)]), [])
        # Other terms still apply to that file ...
        hits = leakcheck.scan(quoted, [(1, "a Carrier build")])
        self.assertEqual([h.term.label for h in hits], ["carrier"])
        # ... and the term still applies to every other file.
        hits = leakcheck.scan("src/a.c", [(1, note)])
        self.assertEqual([h.term.label for h in hits], ["/tmp/"])

    def test_clean_text(self) -> None:
        for text in (
            "a caller",
            "an embedding application",
            "multimapNew()",
            "vertical0",
            "the production path",
            "https://example.com/notes/1234",
            "see docs/notes/x.md",
        ):
            with self.subTest(text=text):
                self.assertEqual(labels(text), [])

    def test_added_lines(self) -> None:
        diff = (
            "diff --git a/x.c b/x.c\n"
            "index 1..2 100644\n"
            "--- a/x.c\n"
            "+++ b/x.c\n"
            "@@ -1,0 +5,2 @@\n"
            "+++counter; /* an added line that starts with ++ */\n"
            "+second\n"
            "@@ -9 +12 @@\n"
            "-old\n"
            "+new\n"
            "diff --git a/gone.c b/gone.c\n"
            "deleted file mode 100644\n"
            "--- a/gone.c\n"
            "+++ /dev/null\n"
            "@@ -1 +0,0 @@\n"
            "-bye\n"
            'diff --git "a/sp\\tace.c" "b/sp\\tace.c"\n'
            '+++ "b/sp\\tace.c"\n'
            "@@ -0,0 +1 @@\n"
            "+tab\n"
        )
        self.assertEqual(
            list(leakcheck.added_lines(diff)),
            [
                ("x.c", 5, "++counter; /* an added line that starts with ++ */"),
                ("x.c", 6, "second"),
                ("x.c", 12, "new"),
                ("sp\tace.c", 1, "tab"),
            ],
        )


class Modes(unittest.TestCase):
    def setUp(self) -> None:
        self._env = IsolatedGitEnv().__enter__()
        self.repo = TempRepo()
        self.repo.write("src/clean.c", "int ok;\n")
        self.repo.write("src/old.c", "/* see notes/ here */\n")
        self.repo.write("deps/lib/v.c", "/* carrier */\n")
        self.repo.write("README.md", "P1 review item\n")
        self.repo.write("other/x.c", "/* zvm, outside the scanned dirs */\n")
        self.base = self.repo.commit(
            "base", "src/clean.c", "src/old.c", "deps/lib/v.c", "README.md", "other/x.c"
        )

    def tearDown(self) -> None:
        self.repo.cleanup()
        self._env.__exit__()

    def run_cli(self, *args: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cli.main(["--repo", str(self.repo.path), "leakcheck", *args])
        return code, out.getvalue(), err.getvalue()

    def test_tree(self) -> None:
        code, out, _ = self.run_cli("--tree")
        self.assertEqual(code, 1)
        self.assertIn("src/old.c:1: [notes/]", out)
        self.assertIn("README.md:1: [P<digit>]", out)
        self.assertNotIn("deps/lib", out)
        self.assertNotIn("other/x.c", out)
        code, out, _ = self.run_cli("--tree", "--include-vendored")
        self.assertIn("deps/lib/v.c:1: [carrier]", out)

    def test_staged_checks_only_added_lines(self) -> None:
        # Existing hits in old.c are not part of the staged diff.
        self.repo.write("src/clean.c", "int ok;\nint fine;\n")
        self.repo.git("add", "--", "src/clean.c")
        code, out, _ = self.run_cli("--staged")
        self.assertEqual(code, 0, out)
        self.assertIn("0 hard hit(s)", out)

        self.repo.write("src/clean.c", "int ok;\nint fine;\n/* kraft */\n")
        self.repo.git("add", "--", "src/clean.c")
        code, out, _ = self.run_cli()
        self.assertEqual(code, 1)
        self.assertIn("src/clean.c:3: [kraft]", out)

        # A review-only hit reports but passes.
        self.repo.write("src/clean.c", "int ok;\n/* fixes P2 */\n")
        self.repo.git("add", "--", "src/clean.c")
        code, out, _ = self.run_cli("--staged")
        self.assertEqual(code, 0)
        self.assertIn("REVIEW", out)

    def test_range(self) -> None:
        self.repo.write("src/new.c", "/* /home/someone */\n")
        self.repo.write("deps/lib/v.c", "/* carrier */\n/* tripwire */\n")
        head = self.repo.commit("next", "src/new.c", "deps/lib/v.c")
        code, out, _ = self.run_cli("--range", f"{self.base}..{head}")
        self.assertEqual(code, 1)
        self.assertIn("src/new.c:1: [/home/]", out)
        self.assertNotIn("deps/", out)
        code, out, _ = self.run_cli("--range", f"{head}..{head}")
        self.assertEqual(code, 0)

    def test_input_errors(self) -> None:
        for args in (("--range", "HEAD"), ("--range", "nosuch..HEAD")):
            with self.subTest(args=args):
                code, out, err = self.run_cli(*args)
                self.assertEqual(code, 2)
                self.assertEqual(out, "")
                self.assertEqual(len(err.strip().splitlines()), 1)
                self.assertNotIn("Traceback", err)
        with self.assertRaises(ToolError):
            leakcheck.check(self.repo.path, "bogus")


if __name__ == "__main__":
    unittest.main()
