"""``dktools handles``: every handle says whether its calls can move it.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import unittest

from dktools import handles


def checked(*headers: tuple[str, str]) -> dict[str, handles.Handle]:
    """Each handle the headers declare, classified against all of them."""
    texts = dict(headers)
    found = [h for path, text in texts.items() for h in handles.handles_in(path, text)]
    handles.classify(found, texts)
    return {h.name: h for h in found}


def problems(text: str) -> list[str]:
    return [p for h in checked(("src/x.h", text)).values() for p in h.problems()]


MOVES = "/* @handle: moves */\ntypedef struct box box;\n"
FIXED = "/* @handle: fixed */\ntypedef struct box box;\n"


class Found(unittest.TestCase):
    def test_an_untagged_opaque_struct_fails(self) -> None:
        self.assertEqual(
            problems("typedef struct box box;\nvoid boxFree(box *b);\n"),
            ["src/x.h: box: no @handle tag (moves, returns or fixed)"],
        )

    def test_a_struct_with_its_body_in_the_header_is_data(self) -> None:
        text = "typedef struct pt { int x; } pt;\ntypedef struct pt pt;\n"
        self.assertEqual(checked(("src/x.h", text)), {})

    def test_an_untagged_scalar_typedef_is_no_handle(self) -> None:
        self.assertEqual(checked(("src/x.h", "typedef uint8_t byteish;\n")), {})

    def test_the_tag_sits_on_the_line_or_in_the_block_above(self) -> None:
        for text in (
            "typedef struct box box; /* @handle: fixed */\n",
            "/* A box.\n * @handle: fixed */\ntypedef struct box box;\n",
            "/* @handle: fixed */\ntypedef uint8_t box;\n",
        ):
            with self.subTest(text=text):
                self.assertEqual(checked(("src/x.h", text))["box"].tag, "fixed")

    def test_a_tag_above_another_typedef_does_not_carry_down(self) -> None:
        text = "/* @handle: fixed */\ntypedef struct a a;\ntypedef struct b b;\n"
        found = checked(("src/x.h", text))
        self.assertEqual((found["a"].tag, found["b"].tag), ("fixed", None))

    def test_an_unknown_tag_fails(self) -> None:
        self.assertEqual(
            problems("/* @handle: floats */\ntypedef struct box box;\n"),
            ["src/x.h: box: unknown tag '@handle: floats'"],
        )


class Moves(unittest.TestCase):
    def test_a_double_pointer_call_satisfies_moves(self) -> None:
        text = MOVES + "bool boxGrow(box **b, size_t n);\n"
        found = checked(("src/x.h", text))["box"]
        self.assertEqual(found.moving, ["boxGrow(b)"])
        self.assertEqual(found.problems(), [])

    def test_moves_without_a_double_pointer_call_fails(self) -> None:
        self.assertEqual(
            problems(MOVES + "void boxFree(box *b);\n"),
            ["src/x.h: box: tagged moves, but no call takes box **"],
        )

    def test_reads_of_an_array_of_handles_do_not_move(self) -> None:
        for proto in (
            "void boxSum(size_t n, box *const *boxes);",
            "void boxSum(size_t n, const box **boxes);",
            "void boxSum(size_t n, const box *const *boxes);",
            "box **boxSplit(box *b);",
        ):
            with self.subTest(proto=proto):
                found = checked(("src/x.h", FIXED + proto + "\n"))["box"]
                self.assertEqual(found.moving, [])

    def test_a_double_pointer_with_const_after_counts(self) -> None:
        found = checked(("src/x.h", MOVES + "void boxGrow(box **const b);\n"))
        self.assertEqual(found["box"].moving, ["boxGrow(b)"])

    def test_a_function_pointer_in_a_struct_counts(self) -> None:
        text = (
            "/* @handle: moves */\ntypedef void slot;\n"
            "typedef struct slotClass {\n"
            "    bool (*insert)(struct slotClass *c, slot **s, int v);\n"
            "    size_t (*bytes)(slot *s);\n"
            "} slotClass;\n"
        )
        found = checked(("src/x.h", text))["slot"]
        self.assertEqual(found.moving, ["(*insert)(s)"])
        self.assertEqual(found.problems(), [])

    def test_calls_in_another_header_count(self) -> None:
        found = checked(
            ("src/box.h", MOVES),
            ("src/boxOps.h", '#include "box.h"\nvoid boxGrow(box **b);\n'),
        )
        self.assertEqual(found["box"].moving, ["boxGrow(b)"])

    def test_test_only_declarations_do_not_count(self) -> None:
        text = MOVES + "#ifdef DATAKIT_TEST\nvoid boxPoke(box **b);\n#endif\n"
        self.assertEqual(
            problems(text), ["src/x.h: box: tagged moves, but no call takes box **"]
        )


class Fixed(unittest.TestCase):
    def test_fixed_with_a_double_pointer_call_fails(self) -> None:
        self.assertEqual(
            problems(FIXED + "void boxGrow(box **b);\n"),
            ["src/x.h: box: tagged fixed, but boxGrow(b) take box **"],
        )

    def test_an_out_parameter_hands_out_a_new_handle(self) -> None:
        for param in ("out", "boxOut", "boxPtr"):
            with self.subTest(param=param):
                text = FIXED + f"bool boxOpen(const char *path, box **{param});\n"
                self.assertEqual(problems(text), [])


class Returns(unittest.TestCase):
    TEXT = "/* @handle: returns */\ntypedef char str;\n"

    def test_a_call_returning_the_new_address_satisfies_returns(self) -> None:
        found = checked(
            ("src/x.h", self.TEXT + "str *strCat(str *s, const char *t);\n")
        )
        self.assertEqual(found["str"].returning, ["strCat"])
        self.assertEqual(found["str"].problems(), [])

    def test_a_constructor_alone_does_not(self) -> None:
        self.assertEqual(
            problems(self.TEXT + "str *strNew(const char *t);\n"),
            ["src/x.h: str: tagged returns, but no call returns a str"],
        )

    def test_a_template_counts_once_per_type_it_stands_for(self) -> None:
        text = (
            "/* @handle: returns */\ntypedef char str;\n"
            "/* @handle: returns */\ntypedef char strc;\n"
            "#if defined(FULL)\n#define S_TYPE str\n"
            "#else\n#define S_TYPE strc\n#endif\n"
            "#define S_CAT DK_NAME(S_TYPE, cat)\n"
            "S_TYPE *S_CAT(S_TYPE *s, const char *t);\n"
        )
        found = checked(("src/x.h", text))
        self.assertEqual(found["str"].returning, ["strcat"])
        self.assertEqual(found["strc"].returning, ["strccat"])


if __name__ == "__main__":
    unittest.main()
