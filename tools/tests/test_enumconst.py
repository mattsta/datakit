"""``dktools enumconst``: which one-constant enums become constexpr int.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from dktools import enumconst


def rewrite(src: str) -> tuple[str, int]:
    out, count = enumconst.rewrite(src.encode())
    return out.decode(), count


class Rewrites(unittest.TestCase):
    def test_block_scope_is_constexpr_int(self) -> None:
        self.assertEqual(
            rewrite("void f(void) {\n    enum { N = 300 };\n    g(N);\n}\n"),
            ("void f(void) {\n    constexpr int N = 300;\n    g(N);\n}\n", 1),
        )

    def test_file_scope_is_static_constexpr_int(self) -> None:
        self.assertEqual(
            rewrite("enum { LIMIT = 1 << 10 };\nint f(void);\n"),
            ("static constexpr int LIMIT = 1 << 10;\nint f(void);\n", 1),
        )

    def test_spacing_and_expressions(self) -> None:
        self.assertEqual(
            rewrite("void f(void) {\n    enum {N_TEXTS=countof(texts)};\n}\n"),
            ("void f(void) {\n    constexpr int N_TEXTS = countof(texts);\n}\n", 1),
        )

    def test_inside_a_macro_body(self) -> None:
        self.assertEqual(
            rewrite("#define T(x) do { enum { N = 200 }; use(x, N); } while (0)\n"),
            ("#define T(x) do { constexpr int N = 200; use(x, N); } while (0)\n", 1),
        )

    def test_other_enums_stay(self) -> None:
        src = (
            "enum { A = 1, B = 2 };\n"
            "enum color { RED = 1 };\n"
            "typedef enum { X = 1 } x;\n"
            "enum : uint8_t { Y = 1 };\n"
            "enum { Z };\n"
        )
        self.assertEqual(rewrite(src), (src, 0))

    def test_comments_and_literals_stay(self) -> None:
        src = '/* enum { N = 1 }; */ const char *s = "enum { N = 1 };";\n'
        self.assertEqual(rewrite(src), (src, 0))


class Files(unittest.TestCase):
    def test_dry_run_changes_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "src").mkdir()
            path = repo / "src" / "a.c"
            path.write_bytes(b"enum { N = 3 };\n")
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                enumconst.run(repo, ("src/a.c",), True)
            self.assertEqual(path.read_bytes(), b"enum { N = 3 };\n")
            self.assertIn("src/a.c: 1", buf.getvalue())


if __name__ == "__main__":
    unittest.main()
