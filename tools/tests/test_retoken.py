"""``dktools retoken``: which occurrences are renamed, and the file walk.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from dktools import retoken
from dktools.gitutil import ToolError


def rename(src: str) -> tuple[str, int]:
    out, count = retoken.retoken(src.encode(), b"_Static_assert", b"static_assert")
    return out.decode(), count


class Renames(unittest.TestCase):
    def test_code_occurrences_change(self) -> None:
        out, count = rename(
            '_Static_assert(1, "x");\nint f(void) {\n    _Static_assert(2, "y");\n}\n'
        )
        self.assertEqual(count, 2)
        self.assertNotIn("_Static_assert", out)

    def test_a_macro_body_is_code(self) -> None:
        out, count = rename("#define CHECK(x) _Static_assert(x, #x)\n")
        self.assertEqual((out, count), ("#define CHECK(x) static_assert(x, #x)\n", 1))

    def test_comments_and_literals_stay(self) -> None:
        src = (
            "/* _Static_assert */ // _Static_assert\n"
            'const char *s = "_Static_assert";\n'
        )
        self.assertEqual(rename(src), (src, 0))

    def test_comments_change_on_request_literals_never(self) -> None:
        src = '/* _Static_assert */ const char *s = "_Static_assert";\n'
        out, count = retoken.retoken(
            src.encode(), b"_Static_assert", b"static_assert", comments=True
        )
        self.assertEqual(
            (out.decode(), count),
            ('/* static_assert */ const char *s = "_Static_assert";\n', 1),
        )

    def test_an_apostrophe_in_a_directive_ends_with_its_line(self) -> None:
        out, count = rename('#error don\'t\n_Static_assert(1, "x");\n')
        self.assertEqual(count, 1)
        self.assertIn("\nstatic_assert(1", out)

    def test_text_changes_only_where_all_of_it_is_code(self) -> None:
        src = (
            b'int a = {0};\n/* b = {0} */\nconst char *s = "= {0}";\n'
            b"#define Z(x) x = {0}\nint c = {0}, d = {0};\n"
        )
        out, count = retoken.retext(src, b"= {0}", b"= {}")
        self.assertEqual(count, 4)
        self.assertEqual(
            out,
            b'int a = {};\n/* b = {0} */\nconst char *s = "= {0}";\n'
            b"#define Z(x) x = {}\nint c = {}, d = {};\n",
        )

    def test_only_whole_tokens(self) -> None:
        src = "my_Static_assert(1); _Static_asserted(2);\n"
        self.assertEqual(rename(src), (src, 0))


class Run(unittest.TestCase):
    def test_given_paths_change_and_dry_run_does_not(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "a.c").write_text("_Thread_local int x;\n")
            with contextlib.redirect_stdout(io.StringIO()) as out:
                retoken.run(repo, "_Thread_local", "thread_local", ("a.c",), True)
            self.assertIn("would rename 1", out.getvalue())
            self.assertEqual((repo / "a.c").read_text(), "_Thread_local int x;\n")
            with contextlib.redirect_stdout(io.StringIO()):
                retoken.run(repo, "_Thread_local", "thread_local", ("a.c",), False)
            self.assertEqual((repo / "a.c").read_text(), "thread_local int x;\n")

    def test_bad_tokens_and_paths_are_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(ToolError):
                retoken.run(Path(tmp), "a-b", "c", ("a.c",), True)
            with self.assertRaises(ToolError):
                retoken.run(Path(tmp), "a", "b", ("missing.c",), True)


if __name__ == "__main__":
    unittest.main()
