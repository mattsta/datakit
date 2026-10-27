"""``dktools testbytes``: which memcmp checks become TEST_BYTES.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from dktools import testbytes
from dktools.gitutil import ToolError


class Rewrite(unittest.TestCase):
    def test_the_single_err_shape_is_rewritten(self) -> None:
        src = (
            b"    if (memcmp(tag, exp_tag, 16) != 0) {\n"
            b'        ERR("tag mismatch");\n'
            b"    }\n"
        )
        out, count = testbytes.rewrite(src)
        self.assertEqual(count, 1)
        self.assertEqual(
            out, b'    TEST_BYTES(err, tag, exp_tag, 16, "tag mismatch");\n'
        )

    def test_format_arguments_and_nested_parentheses_carry_over(self) -> None:
        src = (
            b"if (memcmp(out[(i)], f(a, b), sizeof(out[0])) != 0) {\n"
            b'    ERR("trial %u: n=%zu", t,\n'
            b"        n);\n"
            b"}\n"
        )
        out, count = testbytes.rewrite(src)
        self.assertEqual(count, 1)
        self.assertEqual(
            out,
            b"TEST_BYTES(err, out[(i)], f(a, b), sizeof(out[0]), "
            b'"trial %u: n=%zu", t, n);\n',
        )

    def test_other_shapes_are_left_alone(self) -> None:
        cases = [
            # an else branch
            b'if (memcmp(a, b, 4) != 0) {\n    ERR("x");\n} else {\n}\n',
            # two statements in the body
            b'if (memcmp(a, b, 4) != 0) {\n    ERR("x");\n    break;\n}\n',
            # a different comparison
            b'if (memcmp(a, b, 4) == 0) {\n    ERR("x");\n}\n',
            # another condition joined in
            b'if (memcmp(a, b, 4) != 0 || x) {\n    ERR("x");\n}\n',
            # inside a comment and a string
            b'/* if (memcmp(a, b, 4) != 0) {\n    ERR("x");\n} */\n',
            b'const char *s = "if (memcmp(a, b, 4) != 0) { ERR(x); }";\n',
        ]
        for src in cases:
            with self.subTest(src=src):
                self.assertEqual(testbytes.rewrite(src), (src, 0))

    def test_several_in_one_file(self) -> None:
        one = b'if (memcmp(a, b, 1) != 0) {\n    ERR("a");\n}\n'
        two = b'if (memcmp(c, d, 2) != 0) {\n    ERR("c %d", 2);\n}\n'
        out, count = testbytes.rewrite(one + b"x();\n" + two)
        self.assertEqual(count, 2)
        self.assertEqual(
            out,
            b'TEST_BYTES(err, a, b, 1, "a");\n'
            b"x();\n"
            b'TEST_BYTES(err, c, d, 2, "c %d", 2);\n',
        )


class Run(unittest.TestCase):
    def test_given_paths_change_and_dry_run_does_not(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            text = 'if (memcmp(a, b, 1) != 0) {\n    ERR("a");\n}\n'
            (repo / "t.c").write_text(text)
            with contextlib.redirect_stdout(io.StringIO()) as out:
                testbytes.run(repo, ("t.c",), True)
            self.assertIn("would rewrite 1", out.getvalue())
            self.assertEqual((repo / "t.c").read_text(), text)
            with contextlib.redirect_stdout(io.StringIO()):
                testbytes.run(repo, ("t.c",), False)
            self.assertEqual(
                (repo / "t.c").read_text(), 'TEST_BYTES(err, a, b, 1, "a");\n'
            )

    def test_a_missing_path_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, self.assertRaises(ToolError):
            testbytes.run(Path(tmp), ("nope.c",), True)


if __name__ == "__main__":
    unittest.main()
