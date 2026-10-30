"""``dktools deadsurface``: settings and constants no library code acts on.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from dktools import deadsurface


class Parts(unittest.TestCase):
    def test_field_names_skip_widths_bounds_and_nested_bodies(self) -> None:
        body = (
            " int a; uint32_t b : 3; char name[16]; int c, d;"
            " struct { int inner; } nested; void (*fn)(int);"
        )
        self.assertEqual(deadsurface.field_names(body), ["a", "b", "name", "c", "d"])

    def test_enum_constants_with_and_without_values(self) -> None:
        text = "typedef enum mode { MODE_A = 0, MODE_B, MODE_C = 7 } mode;\n"
        self.assertEqual(
            deadsurface.enum_constants(text), ["MODE_A", "MODE_B", "MODE_C"]
        )

    def test_a_read_is_not_an_assignment_or_a_designator(self) -> None:
        self.assertFalse(
            deadsurface.field_read("c.level = 3; x = (T){.level = 1};", "level")
        )
        self.assertTrue(deadsurface.field_read("if (c->level > 2) {}", "level"))
        self.assertTrue(deadsurface.field_read("use(c.level);", "level"))
        self.assertTrue(deadsurface.field_read("if (c.level == 2) {}", "level"))


class Collect(unittest.TestCase):
    def test_unread_fields_and_unnamed_constants_are_listed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "src").mkdir()
            (repo / "src/x.h").write_text(
                "typedef struct xConfig { int used; int unused; } xConfig;\n"
                "typedef enum xMode { X_ON = 0, X_OFF, X_NEVER } xMode;\n"
            )
            (repo / "src/x.c").write_text(
                '#include "x.h"\n'
                "int f(const xConfig *c) { return c->used + X_ON + X_OFF; }\n"
                "xConfig d = {.unused = 1};\n"
            )
            (repo / "src/xTest.c").write_text(
                "int g(xConfig c) { return c.unused + X_NEVER; }\n"
            )
            found = {(d.kind, d.name) for d in deadsurface.collect(repo)}
        self.assertEqual(found, {("field", "unused"), ("enum", "X_NEVER")})


if __name__ == "__main__":
    unittest.main()
