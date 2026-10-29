"""``dktools structs``: struct bodies public headers show but should not.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import unittest

from dktools import structs


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


if __name__ == "__main__":
    unittest.main()
