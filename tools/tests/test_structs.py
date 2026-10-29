"""``dktools structs``: struct bodies public headers show but should not.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from dktools import structs
from dktools.gitutil import ToolError


def kinds(*headers: tuple[str, str]) -> dict[str, str]:
    texts = dict(headers)
    found = [s for path, text in texts.items() for s in structs.bodies_in(path, text)]
    structs.classify(found, texts)
    return {s.name: s.kind for s in found}


class Bodies(unittest.TestCase):
    def test_tag_and_typedef_names(self) -> None:
        text = (
            "typedef struct a { int x; } a;\n"
            "struct b { int y; };\n"
            "typedef struct { int z; } c;\n"
            "typedef struct d d;\n"
        )
        found = structs.bodies_in("src/x.h", text)
        self.assertEqual([s.names for s in found], [("a",), ("b",), ("c",)])

    def test_test_only_and_commented_bodies_do_not_count(self) -> None:
        text = (
            "/* struct a { int x; }; */\n"
            "#ifdef DATAKIT_TEST\nstruct b { int y; };\n#endif\n"
        )
        self.assertEqual(structs.bodies_in("src/x.h", text), [])


class Kinds(unittest.TestCase):
    def test_a_struct_a_call_returns_by_pointer_is_a_handle(self) -> None:
        text = "typedef struct box { int n; } box;\nbox *boxNew(void);\n"
        self.assertEqual(kinds(("src/x.h", text)), {"box": "handle"})

    def test_a_struct_a_call_takes_is_data(self) -> None:
        text = (
            "typedef struct stats { int n; } stats;\n"
            "void boxStats(const void *b, stats *out);\n"
        )
        self.assertEqual(kinds(("src/x.h", text)), {"stats": "data"})

    def test_a_struct_no_call_names_is_unnamed(self) -> None:
        text = "typedef struct scratch { int n; } scratch;\nvoid f(int x);\n"
        self.assertEqual(kinds(("src/x.h", text)), {"scratch": "unnamed"})

    def test_a_struct_held_by_a_named_struct_is_named(self) -> None:
        text = (
            "typedef struct inner { int n; } inner;\n"
            "typedef struct outer { inner i; } outer;\n"
            "void use(outer *o);\n"
        )
        self.assertEqual(kinds(("src/x.h", text)), {"inner": "data", "outer": "data"})

    def test_inline_definitions_and_other_headers_name_structs(self) -> None:
        self.assertEqual(
            kinds(
                ("src/a.h", "typedef struct pt { int x; } pt;\n"),
                (
                    "src/b.h",
                    "static inline int ptX(const pt *p) {\n    return p->x;\n}\n",
                ),
            ),
            {"pt": "data"},
        )


class Gate(unittest.TestCase):
    def tree(self, header: str, allow: str) -> Path:
        root = Path(self.enterContext(tempfile.TemporaryDirectory()))
        (root / "src").mkdir()
        (root / "src" / "box.h").write_text(header)
        (root / "tools").mkdir()
        (root / "tools" / "structs-allow.txt").write_text(allow)
        return root

    HANDLE = "typedef struct box { int n; } box;\nbox *boxNew(void);\n"

    def test_a_handle_body_fails_unless_allowed(self) -> None:
        lines, bodies = structs.gate(self.tree(self.HANDLE, ""))
        self.assertEqual(bodies, 1)
        self.assertEqual(len(lines), 1)
        self.assertIn("src/box.h: box: handle struct body", lines[0])
        allowed = self.tree(self.HANDLE, "src/box.h box callers size it\n")
        self.assertEqual(structs.gate(allowed), ([], 1))

    def test_a_stale_allow_line_fails(self) -> None:
        header = "typedef struct stats { int n; } stats;\nvoid f(stats *s);\n"
        lines, _ = structs.gate(self.tree(header, "src/box.h stats old reason\n"))
        self.assertEqual(len(lines), 1)
        self.assertIn("allowed but not a handle", lines[0])

    def test_an_allow_line_needs_a_reason(self) -> None:
        with self.assertRaises(ToolError):
            structs.parse_allow("src/box.h box\n")


if __name__ == "__main__":
    unittest.main()
