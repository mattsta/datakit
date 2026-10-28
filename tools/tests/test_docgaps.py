"""``dktools docgaps``: public functions the user-level docs never name.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from dktools import docgaps


class DocGaps(unittest.TestCase):
    def setUp(self) -> None:
        self._dir = tempfile.TemporaryDirectory()
        self.repo = Path(self._dir.name)
        (self.repo / "src").mkdir()
        (self.repo / "docs" / "modules").mkdir(parents=True)
        (self.repo / "src" / "a.h").write_text(
            "#pragma once\nint aOne(int x);\nvoid aTwo(void);\nvoid aThree(void);\n"
        )
        (self.repo / "src" / "b.h").write_text(
            "#pragma once\nvoid bOnly(void);\nint bTest(int argc, char *argv[]);\n"
        )
        # internal headers are not public API
        (self.repo / "src" / "aInternal.h").write_text("void hidden(void);\n")
        (self.repo / "docs" / "modules" / "A.md").write_text(
            "Call `aOne(x)` first; aThreeish is another word.\n"
        )
        (self.repo / "README.md").write_text("bOnly is documented here.\n")

    def tearDown(self) -> None:
        self._dir.cleanup()

    def report(self) -> list[docgaps.HeaderGaps]:
        headers = docgaps.collect_headers(self.repo)
        return docgaps.gaps(headers, docgaps.doc_words(self.repo))

    def test_whole_words_in_docs_and_readme_count_as_documented(self) -> None:
        by = {g.header: g for g in self.report()}
        self.assertEqual(set(by), {"src/a.h", "src/b.h"})
        self.assertEqual(by["src/a.h"].undocumented, ("aThree", "aTwo"))
        self.assertEqual(by["src/a.h"].documented, 1)
        self.assertEqual(by["src/b.h"].undocumented, ())

    def test_the_report_lists_most_undocumented_first_with_totals(self) -> None:
        text = docgaps.render(self.report(), names=True)
        lines = text.splitlines()
        self.assertEqual(lines[0], "    2 of    3  src/a.h")
        self.assertEqual(lines[1:3], ["        aThree", "        aTwo"])
        self.assertEqual(
            lines[-1],
            "docgaps: 2 of 4 public functions undocumented; "
            "1 of 2 headers fully documented; 0 module docs not in docs/INDEX.md",
        )

    def test_module_docs_the_index_does_not_link_are_named(self) -> None:
        (self.repo / "docs" / "INDEX.md").write_text("- [B](modules/B.md)\n")
        (self.repo / "docs" / "modules" / "B.md").write_text("# B\n")
        self.assertEqual(docgaps.unindexed(self.repo), ["docs/modules/A.md"])
        text = docgaps.render(self.report(), False, docgaps.unindexed(self.repo))
        self.assertTrue(text.startswith("  not in docs/INDEX.md: docs/modules/A.md"))

    def test_a_header_that_is_not_public_is_an_input_error(self) -> None:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            code = docgaps.run(self.repo, ("src/aInternal.h",), False)
        self.assertEqual(code, 2)
        self.assertIn("not a public header", out.getvalue())


if __name__ == "__main__":
    unittest.main()
