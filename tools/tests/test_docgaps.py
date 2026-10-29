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
            "#ifdef DATAKIT_TEST\nvoid bTestHook(void);\n#endif\n"
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
        return docgaps.gaps(headers, docgaps.doc_index(self.repo))

    def test_typed_families_are_documented_by_pattern_or_by_a_sibling(self) -> None:
        (self.repo / "src" / "fw.h").write_text(
            "#pragma once\nint fenwickI64Query(void);\nint fenwickU32Query(void);\n"
            "int fenwickU32Bytes(void);\nint fenwick2dU16Get(void);\n"
            "int segmentU16NewFromArray(void);\nint segmentWideFoo(void);\n"
        )
        (self.repo / "docs" / "modules" / "F.md").write_text(
            "`fenwick<T>` trees: fenwickI64Query sums.  "
            "`segment<T>NewFromArray(op, v, n)` builds.\n"
        )
        by = {g.header: g for g in self.report()}
        # U32Query by its I64 sibling, NewFromArray by its pattern; Bytes has
        # no documented sibling, a 2d name is not of the fenwick<T> family,
        # and 'Wide' is not a type tag
        self.assertEqual(
            by["src/fw.h"].undocumented,
            ("fenwick2dU16Get", "fenwickU32Bytes", "segmentWideFoo"),
        )

    def test_system_functions_a_header_redeclares_are_not_api(self) -> None:
        (self.repo / "src" / "sys.h").write_text(
            "#pragma once\nint posix_memalign(void **p, size_t a, size_t n);\n"
            "int pthread_cond_timedwait_relative_np(void *c, void *m, void *t);\n"
            "void sysOwn(void);\n"
        )
        by = {g.header: g for g in self.report()}
        self.assertEqual(by["src/sys.h"].declared, 1)
        self.assertEqual(by["src/sys.h"].undocumented, ("sysOwn",))

    def test_per_structure_calls_document_every_structures_call(self) -> None:
        (self.repo / "src" / "p.h").write_text(
            "#pragma once\nvoid listNewP(void);\nvoid mapNewP(void);\n"
            "void mapSyncP(void);\n"
        )
        (self.repo / "docs" / "modules" / "P.md").write_text(
            "| `<s>NewP(ctx)` | a new structure |\n"
        )
        by = {g.header: g for g in self.report()}
        self.assertEqual(by["src/p.h"].undocumented, ("mapSyncP",))

    def test_tier_headers_are_summed_apart_unless_named(self) -> None:
        (self.repo / "src" / "mm.h").write_text("#pragma once\nvoid mmGet(void);\n")
        (self.repo / "src" / "mmSmall.h").write_text(
            "#pragma once\nvoid mmSmallGet(void);\nvoid mmSmallPut(void);\n"
        )
        # no umbrella: an ordinary header
        (self.repo / "src" / "lonelyFull.h").write_text(
            "#pragma once\nvoid lonelyFullGo(void);\n"
        )
        report = self.report()
        by = {g.header: g for g in report}
        self.assertTrue(by["src/mmSmall.h"].tier)
        self.assertFalse(by["src/lonelyFull.h"].tier)
        text = docgaps.render(report, names=False)
        self.assertNotIn("src/mmSmall.h", text)
        self.assertIn(
            "  tier headers (used through their umbrella, not counted): "
            "1 headers, 2 functions no doc names",
            text,
        )
        # a.h 2, mm.h 1, lonelyFull.h 1 undocumented; b.h complete
        self.assertTrue(
            text.splitlines()[-1].startswith(
                "docgaps: 4 of 6 public functions undocumented; "
                "1 of 4 headers fully documented"
            )
        )

        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            docgaps.run(self.repo, ("src/mmSmall.h",), False)
        self.assertIn("    2 of    2  src/mmSmall.h", out.getvalue())

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
