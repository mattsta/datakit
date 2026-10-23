"""``dktools atomicaudit``: tag grammar, order checks, attachment, edges.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from dktools import atomicaudit, cli


def scan(src: str) -> list[str]:
    _, _, violations = atomicaudit.scan_text("x.c", src)
    return [v.message for v in violations]


class Grammar(unittest.TestCase):
    def test_valid_clauses(self) -> None:
        clauses, err = atomicaudit.parse_tag("/* @sync: acquire a.b; release c */")
        self.assertIsNone(err)
        self.assertEqual(
            clauses,
            [atomicaudit.Clause("acquire", "a.b"), atomicaudit.Clause("release", "c")],
        )

    def test_rejections(self) -> None:
        for text in (
            "/* @sync: relaxed because */",
            "/* @sync: local vibes */",
            "/* @sync: publish x */",
            "/* @sync: release */",
            "/* @sync: relaxed counter; release x */",
            "/* @sync: release x; acquire x */",
            "/* see @sync: release x */",
        ):
            with self.subTest(text=text):
                _, err = atomicaudit.parse_tag(text)
                self.assertIsNotNone(err)


class Orders(unittest.TestCase):
    def test_matching_tags_are_clean(self) -> None:
        src = """
        void f(void) {
            /* @sync: release e */
            atomic_store_explicit(&x, 1, memory_order_release);
            int v = atomic_load_explicit(&x, memory_order_acquire); /* @sync: acquire e */
            /* @sync: relaxed counter */
            atomic_fetch_add_explicit(&n, 1, memory_order_relaxed);
            /* @sync: seq_cst g */
            atomic_fetch_add(&c, 1);
            /* @sync: seq_cst g */
            __atomic_load_n(&c, __ATOMIC_SEQ_CST);
            /* @sync: local timing */
            __sync_synchronize();
        }
        """
        self.assertEqual(scan(src), [])

    def test_under_and_over_ordered(self) -> None:
        under = "/* @sync: acquire e */\nint v = atomic_load_explicit(&x, memory_order_relaxed);\n"
        over = "/* @sync: release e */\natomic_exchange_explicit(&x, 1, memory_order_acq_rel);\n"
        self.assertIn("UNDER-ORDERED", scan(under)[0])
        self.assertIn("OVER-ORDERED", scan(over)[0])

    def test_invalid_orders(self) -> None:
        for src, want in (
            (
                "/* @sync: release e */\natomic_load_explicit(&x, memory_order_release);",
                "a load cannot",
            ),
            (
                "/* @sync: acquire e */\natomic_store_explicit(&x, 1, memory_order_acquire);",
                "a store cannot",
            ),
            (
                "/* @sync: acquire e */\natomic_load_explicit(&x, memory_order_consume);",
                "consume",
            ),
            ("/* @sync: acquire e */\natomic_load_explicit(&x, mo);", "not a constant"),
            (
                "/* @sync: acquire e */\natomic_compare_exchange_strong_explicit("
                "&x, &e, 1, memory_order_acq_rel, memory_order_release);",
                "failure order cannot be release",
            ),
        ):
            with self.subTest(src=src):
                self.assertIn(want, scan(src)[0])

    def test_cas_union_of_success_and_failure(self) -> None:
        src = (
            "/* @sync: release f; acquire r */\n"
            "atomic_compare_exchange_strong_explicit(&w, &e, 1, "
            "memory_order_release, memory_order_acquire);\n"
        )
        self.assertEqual(scan(src), [])


class Attachment(unittest.TestCase):
    def test_untagged_and_orphan(self) -> None:
        msgs = scan(
            "void f(void) { atomic_load(&x); }\n/* @sync: relaxed hint */\nint y;\n"
        )
        self.assertTrue(any("no @sync tag" in m for m in msgs))
        self.assertTrue(any("attached to no atomic" in m for m in msgs))

    def test_tag_does_not_cross_statements(self) -> None:
        src = "/* @sync: relaxed hint */\nint y = 0;\nint v = atomic_load_explicit(&x, memory_order_relaxed);\n"
        msgs = scan(src)
        self.assertTrue(any("no @sync tag" in m for m in msgs))

    def test_multiline_statement_and_condition(self) -> None:
        src = """
        /* @sync: relaxed seed */
        uint64_t prev =
            atomic_load_explicit(&c, memory_order_relaxed);
        /* @sync: release e */
        while (prev < n && !atomic_compare_exchange_weak_explicit(
                               &c, &prev, n, memory_order_release,
                               memory_order_relaxed)) {
        }
        """
        self.assertEqual(scan(src), [])

    def test_macro_definition_and_comments_strings_ignored(self) -> None:
        src = (
            "#define BUMP(p) \\\n"
            "    /* @sync: relaxed counter */ \\\n"
            "    atomic_fetch_add_explicit((p), 1, memory_order_relaxed)\n"
            'const char *s = "atomic_load(&x)"; /* atomic_store(&x, 1) */\n'
        )
        self.assertEqual(scan(src), [])


class Tree(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        (self.repo / "src" / "memshard").mkdir(parents=True)
        (self.repo / "src" / "memshard" / "v.c").write_text(
            "void g(void){atomic_load(&x);}\n"
        )

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def cli(self, *args: str) -> tuple[int, str]:
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
            code = cli.main(["--repo", str(self.repo), "atomicaudit", *args])
        return code, out.getvalue()

    def test_edges_need_both_sides(self) -> None:
        (self.repo / "src" / "a.c").write_text(
            "/* @sync: release lonely */\natomic_store_explicit(&x, 1, memory_order_release);\n"
        )
        code, out = self.cli("check")
        self.assertEqual(code, 1)
        self.assertIn("edge 'lonely' has no observing side", out)

    def test_seq_cst_edge_must_be_uniform(self) -> None:
        (self.repo / "src" / "a.c").write_text(
            "/* @sync: seq_cst g */\natomic_fetch_add(&c, 1);\n"
            "/* @sync: acquire g */\natomic_load_explicit(&c, memory_order_acquire);\n"
        )
        code, out = self.cli("check")
        self.assertEqual(code, 1)
        self.assertIn("mixes seq_cst", out)

    def test_clean_tree_skips_vendored(self) -> None:
        (self.repo / "src" / "a.c").write_text(
            "/* @sync: release e */\natomic_store_explicit(&x, 1, memory_order_release);\n"
            "/* @sync: acquire e */\natomic_load_explicit(&x, memory_order_acquire);\n"
        )
        code, out = self.cli("check")
        self.assertEqual(code, 0, out)
        self.assertIn("2 atomic operations, 2 tagged, 1 edges, 0 violations", out)


if __name__ == "__main__":
    unittest.main()
