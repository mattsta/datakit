"""``dktools spell``: the file set, the words file, reporting and --write.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from dktools import spell
from dktools.gitutil import ToolError
from repofixture import IsolatedGitEnv, TempRepo

#: Misspellings the tests feed codespell (one it has a single fix for, one
#: with two), built from pieces so this file passes the spelling gate
ONE_FIX = "refr" + "ence"
TWO_FIXES = "wi" + "te"


class Spell(unittest.TestCase):
    def setUp(self) -> None:
        self.env = IsolatedGitEnv().__enter__()
        self.repo = TempRepo()
        self.addCleanup(self.repo.cleanup)
        self.addCleanup(self.env.__exit__)

    def run_spell(self, *paths: str, write: bool = False) -> tuple[int, str]:
        with contextlib.redirect_stdout(io.StringIO()) as out:
            code = spell.run(self.repo.path, paths, write)
        return code, out.getvalue()

    def test_the_default_set_is_tracked_sources_docs_and_tools(self) -> None:
        for rel in (
            "src/a.c",
            "src/sub/b.h",
            "src/memshard/c.c",
            "docs/x/d.md",
            "README.md",
            "CMakeLists.txt",
            "src/CMakeLists.txt",
            "tools/src/dktools/e.py",
            "tools/tests/test_f.py",
            "tools/README.md",
            "notes.md",
            "src/data.bin",
        ):
            self.repo.write(rel, "ok\n")
        self.repo.write("src/untracked.c", "ok\n")
        tracked = [
            "src/a.c",
            "src/sub/b.h",
            "src/memshard/c.c",
            "docs/x/d.md",
            "README.md",
            "CMakeLists.txt",
            "src/CMakeLists.txt",
            "tools/src/dktools/e.py",
            "tools/tests/test_f.py",
            "tools/README.md",
            "notes.md",
            "src/data.bin",
        ]
        self.repo.commit("files", *tracked)
        self.assertEqual(
            sorted(spell.targets(self.repo.path)),
            sorted(
                [
                    "CMakeLists.txt",
                    "README.md",
                    "docs/x/d.md",
                    "src/CMakeLists.txt",
                    "src/a.c",
                    "src/sub/b.h",
                    "tools/README.md",
                    "tools/src/dktools/e.py",
                    "tools/tests/test_f.py",
                ]
            ),
        )

    def test_an_exported_tree_is_walked_with_the_same_set(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for rel in (
                "src/a.c",
                "src/sub/b.h",
                "src/memshard/c.c",
                "deps/x/CMakeLists.txt",
                "src/CMakeLists.txt",
                "docs/x/d.md",
                "README.md",
                "tools/README.md",
                "tools/src/dktools/e.py",
                "notes.md",
            ):
                (root / rel).parent.mkdir(parents=True, exist_ok=True)
                (root / rel).write_text("ok\n")
            self.assertEqual(
                spell.targets(root),
                [
                    "README.md",
                    "docs/x/d.md",
                    "src/CMakeLists.txt",
                    "src/a.c",
                    "src/sub/b.h",
                    "tools/README.md",
                    "tools/src/dktools/e.py",
                ],
            )

    def test_a_misspelling_is_reported_and_a_clean_file_is_not(self) -> None:
        self.repo.write("bad.c", f"/* an external {ONE_FIX} */\n")
        self.repo.write("good.c", "/* an external reference */\n")
        code, out = self.run_spell("bad.c", "good.c")
        self.assertEqual(code, 1)
        self.assertIn(f"bad.c:1: {ONE_FIX} ==> reference", out)
        self.assertIn("1 flagged in 1 of 2 files", out)
        self.assertEqual(self.run_spell("good.c")[0], 0)

    def test_the_words_file_accepts_a_word(self) -> None:
        self.repo.write("key.c", 'const char *k = "whats the word";\n')
        self.assertEqual(self.run_spell("key.c")[0], 1)
        self.repo.write(spell.WORDS, "whats\n")
        self.assertEqual(self.run_spell("key.c")[0], 0)

    def test_write_fixes_single_suggestions_and_keeps_the_rest(self) -> None:
        path = self.repo.write("w.c", f"/* a {ONE_FIX}; we {TWO_FIXES} it */\n")
        code, out = self.run_spell("w.c", write=True)
        self.assertEqual(code, 1)
        self.assertIn("fixed 1", out)
        self.assertIn(f"{TWO_FIXES} ==> write, white", out)
        self.assertEqual(path.read_text(), f"/* a reference; we {TWO_FIXES} it */\n")

    def test_a_missing_path_is_refused(self) -> None:
        with self.assertRaises(ToolError):
            self.run_spell("nope.c")


if __name__ == "__main__":
    unittest.main()
