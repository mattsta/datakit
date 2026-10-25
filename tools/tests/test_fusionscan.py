"""``dktools fusionscan``: which read-then-write pairs are listed.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from dktools import cli, fusionscan

KNOWN = sorted(
    ["multimap", "multimapFull", "multiOrderedSet", "flex", "intset"],
    key=len,
    reverse=True,
)


def scan(src: str) -> list[tuple[str, str, str, str]]:
    return [
        (p.function, p.read.name, p.write.name, p.key)
        for p in fusionscan.scan_text("src/x.c", src, KNOWN)
    ]


class Classify(unittest.TestCase):
    def test_longest_family_wins(self) -> None:
        self.assertEqual(
            fusionscan.classify("multiOrderedSetExists", KNOWN),
            ("multiOrderedSet", "Exists"),
        )
        self.assertEqual(
            fusionscan.classify("multimapFullInsertIfAbsent", KNOWN),
            ("multimapFull", "Insert"),
        )
        self.assertEqual(fusionscan.classify("multimapCount", KNOWN), ("multimap", ""))
        self.assertEqual(fusionscan.classify("zmalloc", KNOWN), ("", ""))

    def test_root_skips_operators_and_casts(self) -> None:
        self.assertEqual(fusionscan.root(("&", "m", "-", ">", "m")), "m")
        self.assertEqual(fusionscan.root(("(", "void", "*", ")", "key")), "key")
        self.assertEqual(fusionscan.root(("elements", "[", "0", "]")), "elements")


class Pairs(unittest.TestCase):
    def test_check_then_change_by_one_key_is_listed(self) -> None:
        src = """
        bool f(wrap *w, const databox *row[]) {
            if (multimapExists(w->m, row[0])) {
                return false;
            }
            multimapInsert(&w->m, row);
            return true;
        }
        """
        self.assertEqual(scan(src), [("f", "multimapExists", "multimapInsert", "row")])

    def test_tier_families_match_their_base(self) -> None:
        src = """
        void f(multimapFull *m, const databox *e[]) {
            if (!multimapFullExistsFullWidth(m, e)) {
                multimapInsert(&m, e);
            }
        }
        """
        self.assertEqual(len(scan(src)), 1)

    def test_other_key_other_container_or_family_is_not(self) -> None:
        src = """
        void f(multimap *m, multimap *n, intset *is, databox *a, databox *b) {
            if (multimapExists(m, a)) {
                multimapDelete(&m, b);
                multimapDelete(&n, a);
                intsetRemove(&m, a, NULL);
            }
        }
        """
        self.assertEqual(scan(src), [])

    def test_a_write_at_the_read_result_is_already_fused(self) -> None:
        src = """
        void f(flex **ff, const databox *key, const databox *box) {
            flexEntry *fe = flexFindByType(*ff, key);
            flexReplaceByType(ff, fe, box);
            flexEntry *again = flexFindByType(*ff, key);
            flexDeleteByType(ff, key, again);
        }
        """
        self.assertEqual(scan(src), [])

    def test_write_before_the_read_is_not_a_pair(self) -> None:
        src = """
        void f(intset **is, int64_t v) {
            intsetAdd(is, v, NULL);
            (void)intsetFind(*is, v);
        }
        """
        self.assertEqual(scan(src), [])

    def test_tests_dead_code_macros_and_members_are_skipped(self) -> None:
        src = """
        #define CHECK_THEN_ADD(s, v) do { intsetFind(s, v); intsetAdd(&s, v, 0); } while (0)
        void f(intset *s, struct ops *o, int64_t v) {
        #if 0
            if (!intsetFind(s, v)) { intsetAdd(&s, v, NULL); }
        #endif
            o->intsetFind(s, v);
            intsetAdd(&s, v, NULL);
        }
        #ifdef DATAKIT_TEST
        void t(intset *s, int64_t v) {
            if (!intsetFind(s, v)) { intsetAdd(&s, v, NULL); }
        }
        #endif
        """
        self.assertEqual(scan(src), [])

    def test_each_read_is_listed_once_per_function(self) -> None:
        src = """
        void f(intset *s, int64_t v) {
            if (!intsetFind(s, v)) { intsetAdd(&s, v, NULL); }
            intsetRemove(&s, v, NULL);
        }
        void g(intset *s, int64_t v) {
            if (intsetFind(s, v)) { intsetRemove(&s, v, NULL); }
        }
        """
        self.assertEqual(
            scan(src),
            [("f", "intsetFind", "intsetAdd", "v"), ("g", "intsetFind", "intsetRemove", "v")],
        )


class Command(unittest.TestCase):
    def test_report_over_a_repository(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "src").mkdir()
            (repo / "src" / "intset.h").write_text("void intsetAdd(void);\n")
            pair = (
                "void f(intset *s, int64_t v) {\n"
                "    if (!intsetFind(s, v)) { intsetAdd(&s, v, NULL); }\n"
                "}\n"
            )
            (repo / "src" / "a.c").write_text(pair)
            (repo / "src" / "intsetTest.c").write_text(pair)  # test-only file
            out = io.StringIO()
            err = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = cli.main(["--repo", str(repo), "fusionscan"])
            self.assertEqual(code, 0)
            self.assertIn("1 read-then-write pairs in 1 functions", err.getvalue())
            self.assertIn("| src/a.c | `f` | 2: `intsetFind` | 2: `intsetAdd`", out.getvalue())


if __name__ == "__main__":
    unittest.main()
