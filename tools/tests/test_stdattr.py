"""``dktools stdattr``: __attribute__ becomes the standard [[...]] form.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import unittest

from dktools import stdattr


def convert(src: str, macros: frozenset[str] = frozenset()) -> tuple[str, list[str]]:
    out, report = stdattr.rewrite(src.encode(), {m.encode() for m in macros})
    return out.decode(), report


class Spelling(unittest.TestCase):
    def test_leading_attributes_swap_in_place(self) -> None:
        self.assertEqual(
            convert(
                '__attribute__((target("avx2"), always_inline)) static inline void f(void);\n'
            ),
            (
                '[[gnu::target("avx2"), gnu::always_inline]] static inline void f(void);\n',
                [],
            ),
        )

    def test_next_to_a_standard_attribute(self) -> None:
        self.assertEqual(
            convert("[[noreturn]] __attribute__((cold)) void die(void);\n"),
            ("[[noreturn]] [[gnu::cold]] void die(void);\n", []),
        )

    def test_optnone_is_clang_and_deprecated_is_standard(self) -> None:
        self.assertEqual(
            convert("__attribute__((optnone)) void f(void);\n"),
            ("[[clang::optnone]] void f(void);\n", []),
        )
        self.assertEqual(
            convert("void *malloc(size_t size) __attribute__((deprecated));\n"),
            ("[[deprecated]] void *malloc(size_t size);\n", []),
        )

    def test_a_multi_line_list(self) -> None:
        self.assertEqual(
            convert(
                '__attribute__((target("aes,pclmul"),\n               always_inline)) static void f(void);\n'
            ),
            (
                '[[gnu::target("aes,pclmul"), gnu::always_inline]] static void f(void);\n',
                [],
            ),
        )


class Placement(unittest.TestCase):
    def test_after_specifiers_moves_to_the_declaration_start(self) -> None:
        self.assertEqual(
            convert(
                "static inline __attribute__((always_inline)) uint64_t f(void) {\n}\n"
            ),
            ("[[gnu::always_inline]] static inline uint64_t f(void) {\n}\n", []),
        )
        self.assertEqual(
            convert(
                "int x;\nstatic void __attribute__((constructor)) init(void) {\n}\n"
            ),
            ("int x;\n[[gnu::constructor]] static void init(void) {\n}\n", []),
        )

    def test_a_trailing_declarator_attribute_moves_too(self) -> None:
        self.assertEqual(
            convert(
                "T *fmt(T *s, const char *f, ...)\n    __attribute__((format(printf, 2, 3)));\n"
            ),
            ("[[gnu::format(printf, 2, 3)]] T *fmt(T *s, const char *f, ...);\n", []),
        )

    def test_a_type_attribute_after_struct_stays(self) -> None:
        self.assertEqual(
            convert(
                "typedef struct __attribute__((packed)) row {\n    int a;\n} row;\n"
            ),
            ("typedef struct [[gnu::packed]] row {\n    int a;\n} row;\n", []),
        )

    def test_aligned_on_a_variable_is_alignas(self) -> None:
        self.assertEqual(
            convert(
                "void f(void) {\n    uint32_t staging[8][8] __attribute__((aligned(32)));\n}\n"
            ),
            ("void f(void) {\n    alignas(32) uint32_t staging[8][8];\n}\n", []),
        )

    def test_after_a_struct_body_is_left_for_a_person(self) -> None:
        src = "typedef struct {\n    int a;\n} __attribute__((packed)) hdr;\n"
        out, report = convert(src)
        self.assertEqual(out, src)
        self.assertEqual(len(report), 1)
        self.assertIn(stdattr.AFTER_BODY, report[0])

    def test_after_a_function_body_is_a_new_declaration(self) -> None:
        self.assertEqual(
            convert(
                'void f(void) {\n}\n\n__attribute__((target("aes"))) static void g(void) {\n}\n'
            ),
            (
                'void f(void) {\n}\n\n[[gnu::target("aes")]] static void g(void) {\n}\n',
                [],
            ),
        )

    def test_a_macro_body_reorders_within_itself(self) -> None:
        self.assertEqual(
            convert("#define HOT static inline __attribute__((always_inline))\n"),
            ("#define HOT [[gnu::always_inline]] static inline\n", []),
        )
        self.assertEqual(
            convert("#define cleaner(fn) __attribute__((cleanup(fn##_cleanup)))\n"),
            ("#define cleaner(fn) [[gnu::cleanup(fn##_cleanup)]]\n", []),
        )

    def test_a_preprocessor_line_ends_the_previous_declaration(self) -> None:
        src = (
            "#define likely_(x) __builtin_expect(!!(x), 1)\n"
            "static inline __attribute__((always_inline)) uint64_t f(void) {\n}\n"
            "#ifndef strdup\n"
            "char *strdup(const char *s) __attribute__((deprecated));\n"
            "#endif\n"
            "#define LOOP(x) do { x; } while (0)\n"
            "static inline __attribute__((always_inline)) void g(void) {\n}\n"
        )
        self.assertEqual(
            convert(src)[0],
            "#define likely_(x) __builtin_expect(!!(x), 1)\n"
            "[[gnu::always_inline]] static inline uint64_t f(void) {\n}\n"
            "#ifndef strdup\n"
            "[[deprecated]] char *strdup(const char *s);\n"
            "#endif\n"
            "#define LOOP(x) do { x; } while (0)\n"
            "[[gnu::always_inline]] static inline void g(void) {\n}\n",
        )

    def test_a_continued_macro_attribute(self) -> None:
        self.assertEqual(
            convert(
                "#define NO_WRAP                                \\\n"
                "    __attribute__((                            \\\n"
                '        no_sanitize("unsigned-integer-overflow")))\n'
            )[0],
            "#define NO_WRAP                                \\\n"
            '    [[gnu::no_sanitize("unsigned-integer-overflow")]]\n',
        )

    def test_consecutive_attributes_in_a_macro_keep_their_order(self) -> None:
        self.assertEqual(
            convert(
                "#define ATTRS                    \\\n"
                '    __attribute__((no_sanitize("integer"))) \\\n'
                "    __attribute__((optnone))\n"
            )[0],
            "#define ATTRS                    \\\n"
            '    [[gnu::no_sanitize("integer")]] \\\n'
            "    [[clang::optnone]]\n",
        )

    def test_comments_and_literals_stay(self) -> None:
        src = '/* __attribute__((cold)) */ const char *s = "__attribute__((cold))";\n'
        self.assertEqual(convert(src), (src, []))


class MacroUses(unittest.TestCase):
    def test_an_attribute_macro_moves_ahead_of_specifiers(self) -> None:
        self.assertEqual(
            convert(
                "DK_STATIC FN_CONST flexEncoding f(int v) {\n}\n"
                "flexEntry FN_PURE *tail(const flex *f) {\n}\n",
                frozenset({"FN_CONST", "FN_PURE"}),
            ),
            (
                "FN_CONST DK_STATIC flexEncoding f(int v) {\n}\n"
                "FN_PURE flexEntry *tail(const flex *f) {\n}\n",
                [],
            ),
        )

    def test_macro_calls_without_semicolons_are_not_specifiers(self) -> None:
        src = (
            "synth(8, uint32_t, Set, )\nsynth(16, uint32_t, Set, )\n"
            "FN_UNUSED static void guard_(void) {\n}\n"
            "synth(8, uint32_t, Set, )\nint f(void) __attribute__((cold));\n"
        )
        out, _ = convert(src, frozenset({"FN_UNUSED"}))
        self.assertIn("FN_UNUSED static void guard_(void) {", out)
        self.assertIn("int f(void) __attribute__((cold));", out)

    def test_attribute_macros_already_leading_stay(self) -> None:
        src = "FN_UNUSED FN_PURE INLINE_ALWAYS uint64_t pow2(uint64_t x) {\n}\n"
        self.assertEqual(convert(src, frozenset({"FN_UNUSED", "FN_PURE"})), (src, []))

    def test_a_function_like_attribute_macro_moves_with_its_arguments(self) -> None:
        self.assertEqual(
            convert(
                "void f(void) {\n    const cleaner(zfree) char *p = zmalloc(1);\n}\n",
                frozenset({"cleaner"}),
            ),
            ("void f(void) {\n    cleaner(zfree) const char *p = zmalloc(1);\n}\n", []),
        )


class Macros(unittest.TestCase):
    def test_attribute_macros_are_the_ones_whose_body_is_only_attributes(self) -> None:
        texts = [
            b"#define FN_PURE [[gnu::pure]]\n"
            b"#define HOT [[gnu::always_inline]] static inline\n"
            b"#define cleaner(fn) [[gnu::cleanup(fn##_cleanup)]]\n"
            b'#define SANE [[gnu::no_sanitize("x")]] [[clang::optnone]]\n'
            b"#define N 3\n"
        ]
        self.assertEqual(
            stdattr.attribute_macros(texts),
            {b"FN_PURE", b"cleaner", b"SANE"},
        )


if __name__ == "__main__":
    unittest.main()
