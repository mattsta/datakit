"""``dktools format``: which files, check vs write, and the gate line.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import unittest
from pathlib import Path

from dktools import cli, formatting
from repofixture import IsolatedGitEnv, TempRepo

REPO_STYLE = (Path(__file__).resolve().parents[2] / ".clang-format").read_text()

#: Each badly formatted file and what its formatter makes of it.
BAD_C = "int  f(int x){if(x)return 1;return 0;}\n"
BAD_MD = "# Title\n*   item\n"
GOOD_MD = "# Title\n\n- item\n"
BAD_PY = "import os\nimport sys\nx=os.sep\n"  # sys unused (F401), spacing
GOOD_PY = "import os\n\nx = os.sep\n"
#: os.path.exists: PTH110, which ruff reports but cannot fix.
UNFIXABLE_PY = 'import os\n\nok = os.path.exists("x")\n'


class FormatRepo(unittest.TestCase):
    def setUp(self) -> None:
        self._env = IsolatedGitEnv().__enter__()
        self.repo = TempRepo()
        self.root = self.repo.path
        self.repo.write(".clang-format", REPO_STYLE)
        self.repo.write(".gitignore", "ignored/\n")
        self.repo.write("src/good.c", "int f(void) {\n    return 0;\n}\n")
        self.repo.write("deps/vendored.c", BAD_C)
        self.repo.write("README.md", GOOD_MD)
        self.repo.commit(
            "base", ".clang-format", ".gitignore", "src/good.c", "deps/vendored.c"
        )
        self.repo.commit("readme", "README.md")

    def tearDown(self) -> None:
        self.repo.cleanup()
        self._env.__exit__()

    def bad_files(self) -> dict[str, str]:
        files = {"src/bad.c": BAD_C, "docs/bad.md": BAD_MD, "tools/bad.py": BAD_PY}
        for rel, text in files.items():
            self.repo.write(rel, text)
        return files

    def text(self, rel: str) -> str:
        return (self.root / rel).read_text()


class Selection(FormatRepo):
    def test_surface_groups_by_formatter_and_skips_deps(self) -> None:
        groups = formatting.surface(
            ["b.c", "a.h", "deps/x.c", "x.md", "y.py", "CMakeLists.txt", "a.h"]
        )
        self.assertEqual(
            groups,
            {"clang-format": ["a.h", "b.c"], "prettier": ["x.md"], "ruff": ["y.py"]},
        )

    def test_changed_files_are_modified_and_untracked_not_ignored(self) -> None:
        self.repo.write("src/good.c", "int f(void) {\n    return 1;\n}\n")
        self.repo.write("new.md", GOOD_MD)
        self.repo.write("ignored/skip.md", BAD_MD)
        (self.root / "README.md").unlink()  # deleted: nothing to format
        self.assertEqual(
            sorted(formatting.changed_files(self.root)), ["new.md", "src/good.c"]
        )
        self.assertIn("deps/vendored.c", formatting.all_files(self.root))


class CheckAndWrite(FormatRepo):
    def test_check_reports_and_changes_nothing(self) -> None:
        files = self.bad_files()
        report = formatting.check(self.root, formatting.all_files(self.root))
        self.assertEqual(
            report.changed,
            {
                "clang-format": ["src/bad.c"],
                "prettier": ["docs/bad.md"],
                "ruff": ["tools/bad.py"],
            },
        )
        # the unused import is a lint finding ruff can fix: not a problem
        self.assertEqual(report.problems, {}, "\n".join(report.log))
        for rel, text in files.items():
            self.assertEqual(self.text(rel), text)
        self.assertEqual(self.text("deps/vendored.c"), BAD_C)
        self.assertTrue(formatting.gate_line(report).startswith("FAILED (3 files;"))

    def test_write_formats_then_check_is_clean(self) -> None:
        self.bad_files()
        report = formatting.write(self.root, formatting.changed_files(self.root))
        self.assertTrue(report.ok, report.log)
        self.assertEqual(
            report.changed,
            {
                "clang-format": ["src/bad.c"],
                "prettier": ["docs/bad.md"],
                "ruff": ["tools/bad.py"],
            },
        )
        self.assertEqual(self.text("docs/bad.md"), GOOD_MD)
        self.assertEqual(self.text("tools/bad.py"), GOOD_PY)
        self.assertIn("if (x) {\n", self.text("src/bad.c"))  # InsertBraces
        self.assertEqual(self.text("deps/vendored.c"), BAD_C)
        again = formatting.check(self.root, formatting.all_files(self.root))
        self.assertTrue(again.clean(), again.log)
        self.assertEqual(formatting.gate_line(again), "ok (5 files)")

    def test_a_finding_ruff_cannot_fix_fails_both_modes(self) -> None:
        self.repo.write("tools/unfixable.py", UNFIXABLE_PY)
        for run in (formatting.check, formatting.write):
            with self.subTest(run=run.__name__):
                report = run(self.root, ["tools/unfixable.py"])
                self.assertEqual(report.problems, {"ruff": ["tools/unfixable.py"]})
                self.assertFalse(report.ok)
        self.assertEqual(self.text("tools/unfixable.py"), UNFIXABLE_PY)

    def test_a_named_gitignored_file_is_formatted_prettierignore_still_holds(
        self,
    ) -> None:
        # .gitignore says what git tracks, not what prettier formats: an
        # internal note named explicitly is formatted; .prettierignore
        # still excludes what it names
        self.repo.write("ignored/note.md", BAD_MD)
        self.repo.write(".prettierignore", "skipped/\n")
        self.repo.write("skipped/away.md", BAD_MD)
        paths = ["ignored/note.md", "skipped/away.md"]
        report = formatting.check(self.root, paths)
        self.assertEqual(report.changed, {"prettier": ["ignored/note.md"]})
        report = formatting.write(self.root, paths)
        self.assertEqual(report.changed, {"prettier": ["ignored/note.md"]})
        self.assertEqual(self.text("ignored/note.md"), GOOD_MD)
        self.assertEqual(self.text("skipped/away.md"), BAD_MD)


class Command(FormatRepo):
    def dktools(self, *argv: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = cli.main(["--repo", str(self.root), "format", *argv])
        return code, out.getvalue()

    def test_check_fails_write_fixes(self) -> None:
        self.bad_files()
        self.repo.write("tools/bad.py", GOOD_PY)
        code, out = self.dktools("check")
        self.assertEqual(code, 1)
        self.assertIn("format check: FAILED (2 files;", out)
        code, out = self.dktools("write")
        self.assertEqual(code, 0)
        self.assertIn("format write: ok (3 files, 2 rewritten)", out)
        code, out = self.dktools("check", "--all")
        self.assertEqual(code, 0, out)

    def test_named_paths_must_be_files(self) -> None:
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(self.dktools("check", "nope.c")[0], 2)
            self.assertEqual(self.dktools("check", "--all", "README.md")[0], 2)
        code, out = self.dktools("check", "README.md")
        self.assertEqual(code, 0)
        self.assertIn("ok (1 files)", out)


if __name__ == "__main__":
    unittest.main()
