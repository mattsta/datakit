"""``dktools countof``: which element counts become countof(X).

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from dktools import countof


def rewrite(src: str) -> tuple[str, int]:
    out, count = countof.rewrite(src.encode())
    return out.decode(), count


class Rewrites(unittest.TestCase):
    def test_both_same_operand_forms(self) -> None:
        self.assertEqual(
            rewrite("n = sizeof(v) / sizeof(*v);\nm = sizeof(w) / sizeof(w[0]);\n"),
            ("n = countof(v);\nm = countof(w);\n", 2),
        )

    def test_spacing_parentheses_and_no_parentheses(self) -> None:
        self.assertEqual(
            rewrite(
                "a = (sizeof(v) / sizeof(v[0]));\n"
                "b = sizeof v / sizeof v[0];\n"
                "c = sizeof( v )/sizeof( *v );\n"
                "d = sizeof(v) / sizeof((v)[0]);\n"
            ),
            (
                "a = (countof(v));\nb = countof(v);\nc = countof(v);\n"
                "d = countof(v);\n",
                4,
            ),
        )

    def test_member_and_row_operands(self) -> None:
        self.assertEqual(
            rewrite(
                "a = sizeof(s.arr) / sizeof(s.arr[0]);\n"
                "b = sizeof(p->v) / sizeof(*p->v);\n"
                "c = sizeof(grid[i]) / sizeof(grid[i][0]);\n"
            ),
            ("a = countof(s.arr);\nb = countof(p->v);\nc = countof(grid[i]);\n", 3),
        )

    def test_count_array_becomes_countof(self) -> None:
        self.assertEqual(
            rewrite("for (i = 0; i < COUNT_ARRAY(cases); i++) {}\n"),
            ("for (i = 0; i < countof(cases); i++) {}\n", 1),
        )

    def test_other_ratios_stay(self) -> None:
        src = (
            "a = sizeof(buf) / sizeof(uint8_t);\n"
            "b = sizeof(v) / sizeof(w[0]);\n"
            "c = sizeof(v) / sizeof(*w);\n"
            "d = sizeof(v) / sizeof(v[1]);\n"
        )
        self.assertEqual(rewrite(src), (src, 0))

    def test_comments_and_literals_stay(self) -> None:
        src = '/* sizeof(v) / sizeof(*v) */ s = "sizeof(v) / sizeof(v[0])";\n'
        self.assertEqual(rewrite(src), (src, 0))

    def test_countof_and_count_array_definitions_stay(self) -> None:
        # rewriting either would define the macro as itself
        src = (
            "#define countof(a) (sizeof(a) / sizeof((a)[0]) + DK_CHECK_(a))\n"
            "#ifndef COUNT_ARRAY\n"
            "#define COUNT_ARRAY(x) (sizeof(x) / sizeof(*(x)))\n"
            "#endif\n"
            "n = sizeof(v) / sizeof(*v);\n"
        )
        out, count = rewrite(src)
        self.assertEqual(count, 1)
        self.assertEqual(
            out, src.replace("n = sizeof(v) / sizeof(*v);", "n = countof(v);")
        )

    def test_a_macro_body_is_code(self) -> None:
        self.assertEqual(
            rewrite("#define N(x) (sizeof(x) / sizeof((x)[0]))\n"),
            ("#define N(x) (countof(x))\n", 1),
        )


class Files(unittest.TestCase):
    def test_dry_run_changes_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "src").mkdir()
            path = repo / "src" / "a.c"
            path.write_bytes(b"n = sizeof(v) / sizeof(*v);\n")
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                countof.run(repo, ("src/a.c",), True)
            self.assertEqual(path.read_bytes(), b"n = sizeof(v) / sizeof(*v);\n")
            self.assertIn("src/a.c: 1", buf.getvalue())
            with contextlib.redirect_stdout(io.StringIO()):
                countof.run(repo, ("src/a.c",), False)
            self.assertEqual(path.read_bytes(), b"n = countof(v);\n")


if __name__ == "__main__":
    unittest.main()
