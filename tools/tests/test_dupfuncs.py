"""``dktools dupfuncs``: which function bodies count as the same.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from dktools import dupfuncs
from dktools.gitutil import ToolError

BODY = b"{\n    int total = 0;\n    for (int i = 0; i < n; i++) {\n        total += v[i];\n    }\n    return total;\n}\n"


class Definitions(unittest.TestCase):
    def test_functions_are_found_and_other_braces_are_not(self) -> None:
        src = (
            b"struct s { int a; };\n"
            b"static const int t[] = {1, 2};\n"
            b"static int sum(const int *v, int n) " + BODY + b"enum e { A, B };\n"
            b"[[nodiscard]] int\nother(void) {\n    return 1;\n}\n"
        )
        names = [name for _, name, _ in dupfuncs.definitions(src)]
        self.assertEqual(names, [b"sum", b"other"])

    def test_braces_in_comments_literals_and_directives_do_not_count(self) -> None:
        src = (
            b"#define OPEN {\n"
            b"/* } { */\n"
            b'static const char *s = "{";\n'
            b"int f(void) {\n    return '}';\n}\n"
        )
        found = dupfuncs.definitions(src)
        self.assertEqual([name for _, name, _ in found], [b"f"])
        self.assertIn(b"'}'", found[0][2])

    def test_tokens_ignore_layout_and_comments_but_keep_literals(self) -> None:
        a = b"int f(void) {\n    return  g(1,2); /* why */\n}\n"
        b = b"int h(void)\n{ return g( 1, 2 );\n}\n"
        c = b"int k(void) { return g(1, 3); }\n"
        (_, _, ta), (_, _, tb), (_, _, tc) = (
            dupfuncs.definitions(a)[0],
            dupfuncs.definitions(b)[0],
            dupfuncs.definitions(c)[0],
        )
        self.assertEqual(ta, tb)
        self.assertNotEqual(ta, tc)


class Duplicates(unittest.TestCase):
    def test_copies_across_files_group_and_small_bodies_drop(self) -> None:
        one = b"static int sum(const int *v, int n) " + BODY
        two = b"/* copy */\nstatic int total(const int *v, int n)\n" + BODY
        tiny = b"int a(void) { return 0; }\nint b(void) { return 0; }\n"
        files = {"one.c": one, "two.c": two, "tiny.c": tiny}
        groups = dupfuncs.duplicates(files, min_tokens=10)
        self.assertEqual(len(groups), 1)
        count, places = groups[0]
        self.assertGreaterEqual(count, 10)
        self.assertEqual(
            [(p.path, p.line, p.name) for p in places],
            [("one.c", 1, "sum"), ("two.c", 2, "total")],
        )
        self.assertEqual(len(dupfuncs.duplicates(files, min_tokens=3)), 2)


RENAMED = (
    b"{\n    int acc = 7;\n    for (int k = 0; k < len; k++) {\n"
    b"        acc += xs[k];\n    }\n    return acc;\n}\n"
)


class Shape(unittest.TestCase):
    def test_renamed_names_and_constants_share_a_shape(self) -> None:
        files = {
            "a.c": b"static int sum(const int *v, int n) " + BODY,
            "b.c": b"static long tot(const int *xs, int len) " + RENAMED,
        }
        self.assertEqual(dupfuncs.duplicates(files, min_tokens=10), [])
        groups = dupfuncs.duplicates(files, min_tokens=10, by_shape=True)
        self.assertEqual(len(groups), 1)
        self.assertEqual([p.name for p in groups[0][1]], ["sum", "tot"])

    def test_another_operator_or_arity_is_another_shape(self) -> None:
        other_op = BODY.replace(b"total += v[i]", b"total -= v[i]")
        other_arity = BODY.replace(b"return total;", b"return f(total, 1);")
        for variant in (other_op, other_arity):
            with self.subTest(variant=variant):
                files = {
                    "a.c": b"int sum(const int *v, int n) " + BODY,
                    "b.c": b"int add(const int *v, int n) " + variant,
                }
                self.assertEqual(
                    dupfuncs.duplicates(files, min_tokens=10, by_shape=True), []
                )

    def test_exact_copies_stay_in_the_exact_list_only(self) -> None:
        files = {
            "a.c": b"int sum(const int *v, int n) " + BODY,
            "b.c": b"int add(const int *v, int n) " + BODY,
        }
        self.assertEqual(len(dupfuncs.duplicates(files, min_tokens=10)), 1)
        self.assertEqual(dupfuncs.duplicates(files, min_tokens=10, by_shape=True), [])

    def test_keywords_and_types_are_kept(self) -> None:
        self.assertEqual(
            dupfuncs.shape((b"uint64_t", b"x", b"=", b"y", b"+", b"0x10", b";")),
            (b"uint64_t", b"ID0", b"=", b"ID1", b"+", b"NUM", b";"),
        )


class Run(unittest.TestCase):
    def test_report_and_missing_path(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "a.c").write_bytes(b"int sum(const int *v, int n) " + BODY)
            (repo / "b.c").write_bytes(b"int add(const int *v, int n) " + BODY)
            with contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertEqual(dupfuncs.run(repo, ("a.c", "b.c"), 10, False), 0)
            self.assertIn("2 copies:", out.getvalue())
            self.assertIn("a.c:1: sum", out.getvalue())
            with self.assertRaises(ToolError):
                dupfuncs.run(repo, ("nope.c",), 10, False)


if __name__ == "__main__":
    unittest.main()
