"""dropargs: cutting call arguments to the kept positions.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import unittest

from dktools import dropargs
from dktools.gitutil import ToolError


class Drop(unittest.TestCase):
    def test_keeps_the_listed_positions(self) -> None:
        src = b"void t(void) {\n    f(ml, 1, 2, 3, 4);\n    f(x,y,z,w,v);\n}\n"
        out, count, bad = dropargs.drop(src, "f", (0, 2), 5)
        self.assertEqual(bad, [])
        self.assertEqual(count, 2)
        self.assertEqual(out, b"void t(void) {\n    f(ml, 2);\n    f(x, z);\n}\n")

    def test_nested_commas_stay_inside_their_argument(self) -> None:
        src = b"f(g(a, b), {1, 2}, s[i, j], \"x, y\", 'c', 5);\n"
        out, count, _ = dropargs.drop(src, "f", (0, 3), 6)
        self.assertEqual(count, 1)
        self.assertEqual(out, b'f(g(a, b), "x, y");\n')

    def test_comments_and_literals_do_not_split(self) -> None:
        src = b'f(a /* , */, "(" , b);\n'
        out, count, _ = dropargs.drop(src, "f", (2,), 3)
        self.assertEqual(count, 1)
        self.assertEqual(out, b"f(b);\n")

    def test_another_arity_leaves_the_file_alone(self) -> None:
        src = b"f(1, 2, 3);\nf(1, 2);\n"
        out, count, bad = dropargs.drop(src, "f", (0,), 3)
        self.assertEqual(out, src)
        self.assertEqual(count, 0)
        self.assertEqual([(m.line, m.arity) for m in bad], [(2, 2)])

    def test_only_whole_identifiers_in_code(self) -> None:
        src = (
            b"#define f(a, b) g(a)\n"
            b"/* f(1, 2) */ ff(1, 2); xf(1, 2);\n"
            b'const char *s = "f(1, 2)";\n'
            b"f (1, 2);\n"
        )
        out, count, bad = dropargs.drop(src, "f", (1,), 2)
        self.assertEqual(bad, [])
        self.assertEqual(count, 1)
        self.assertTrue(out.endswith(b"f (2);\n"))
        self.assertTrue(
            out.startswith(b"#define f(a, b) g(a)\n/* f(1, 2) */ ff(1, 2);")
        )

    def test_a_nested_call_of_the_name_is_left_as_written(self) -> None:
        src = b"f(f(1, 2), 3);\n"
        out, count, _ = dropargs.drop(src, "f", (0,), 2)
        self.assertEqual(count, 1)
        self.assertEqual(out, b"f(f(1, 2));\n")

    def test_multiline_calls(self) -> None:
        src = b"f(ml,\n  16,\n  500,\n  32,\n  20);\n"
        out, count, _ = dropargs.drop(src, "f", (0, 2), 5)
        self.assertEqual(count, 1)
        self.assertEqual(out, b"f(ml, 500);\n")


class Keep(unittest.TestCase):
    def test_positions_must_be_ascending_distinct_and_in_range(self) -> None:
        self.assertEqual(dropargs.parse_keep("0,2", 5), (0, 2))
        for bad in ("2,0", "1,1", "5", "-1", "a"):
            with self.assertRaises(ToolError, msg=bad):
                dropargs.parse_keep(bad, 5)


if __name__ == "__main__":
    unittest.main()
