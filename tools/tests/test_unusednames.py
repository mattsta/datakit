"""``dktools unusednames``: which silenced names lose their silencer, how.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from dktools import unusednames
from dktools.gitutil import ToolError


def convert(src: str) -> tuple[str, int, list[str]]:
    out, count, refusals = unusednames.unname(src.encode())
    return out.decode(), count, [r.reason for r in refusals]


class Converts(unittest.TestCase):
    def test_a_silenced_parameter_loses_its_name_and_the_silencer_goes(self) -> None:
        src = (
            "static bool f(void *ctx, const databox *current, databox *out) {\n"
            "    (void)current;\n"
            "    return g(ctx, out);\n"
            "}\n"
        )
        self.assertEqual(
            convert(src),
            (
                "static bool f(void *ctx, const databox *, databox *out) {\n"
                "    return g(ctx, out);\n"
                "}\n",
                1,
                [],
            ),
        )

    def test_every_silenced_parameter_of_a_function(self) -> None:
        src = (
            "int f(multidictClass *qdc, const databox *keybox, bool take) {\n"
            "    (void)qdc;\n"
            "    (void)keybox;\n"
            "    DK_NOTUSED(take);\n"
            "    return 0;\n"
            "}\n"
        )
        out, count, refusals = convert(src)
        self.assertEqual(count, 3)
        self.assertEqual(refusals, [])
        self.assertEqual(
            out, "int f(multidictClass *, const databox *, bool) {\n    return 0;\n}\n"
        )

    def test_the_unused_macro_and_spacing_inside_the_cast(self) -> None:
        src = "void f(int a, int b) {\n    UNUSED(a);\n    ( void ) b ;\n}\n"
        self.assertEqual(convert(src), ("void f(int, int) {\n}\n", 2, []))

    def test_an_array_parameter_keeps_its_bound(self) -> None:
        src = "void f(const uint8_t key[32]) {\n    (void)key;\n}\n"
        self.assertEqual(convert(src)[:2], ("void f(const uint8_t[32]) {\n}\n", 1))

    def test_a_signature_over_several_lines(self) -> None:
        src = (
            "DK_STATIC int64_t mmSlotInsert(multidictClass *qdc,\n"
            "                               const databox *keybox,\n"
            "                               databox *replaced) {\n"
            "    (void)qdc;\n"
            "    return h(keybox, replaced);\n"
            "}\n"
        )
        out, count, _ = convert(src)
        self.assertEqual(count, 1)
        self.assertIn("mmSlotInsert(multidictClass *,\n", out)
        self.assertNotIn("(void)", out)

    def test_a_silencer_in_a_nested_block(self) -> None:
        src = "void f(int a) {\n    if (g()) {\n        (void)a;\n    }\n}\n"
        self.assertEqual(
            convert(src), ("void f(int) {\n    if (g()) {\n    }\n}\n", 1, [])
        )

    def test_a_silencer_sharing_its_line_leaves_the_rest(self) -> None:
        src = "void f(int a) { (void)a; g(); }\n"
        self.assertEqual(convert(src), ("void f(int) { g(); }\n", 1, []))

    def test_a_parameter_the_comment_above_names_stays_named(self) -> None:
        src = (
            "/* 'hint' is the caller's guess; this class ignores it */\n"
            "static int f(int hint, int x) {\n"
            "    (void)hint;\n"
            "    return x;\n"
            "}\n"
        )
        self.assertEqual(
            convert(src)[:2],
            (
                "/* 'hint' is the caller's guess; this class ignores it */\n"
                "static int f([[maybe_unused]] int hint, int x) {\n"
                "    return x;\n"
                "}\n",
                1,
            ),
        )

    def test_a_body_with_branches_that_never_name_it(self) -> None:
        src = (
            "int fTest(int argc, char *argv[]) {\n"
            "    (void)argc;\n"
            "    (void)argv;\n"
            "#ifdef X\n"
            "    g();\n"
            "#endif\n"
            "    return 0;\n"
            "}\n"
        )
        out, count, refusals = convert(src)
        self.assertEqual((count, refusals), (2, []))
        self.assertTrue(out.startswith("int fTest(int, char *[]) {\n#ifdef X\n"))

    def test_braces_repeated_across_branches_count_once(self) -> None:
        src = (
            "void f(int v) {\n"
            "#if A\n"
            "    if (p(v)) {\n"
            "#else\n"
            "    if (q(v)) {\n"
            "#endif\n"
            "    }\n"
            "}\n"
            "void g(int a) {\n"
            "    (void)a;\n"
            "}\n"
        )
        out, count, refusals = convert(src)
        self.assertEqual((count, refusals), (1, []))
        self.assertTrue(out.endswith("void g(int) {\n}\n"))

    def test_an_if_0_branch_is_not_code_its_else_is(self) -> None:
        src = (
            "void f(int v) {\n"
            "#if 0\n"
            "    if (v) {\n"
            "#  if X\n"
            "    {\n"
            "#  endif\n"
            "#else\n"
            "    {\n"
            "    }\n"
            "#endif\n"
            "}\n"
            "void g(int a) {\n"
            "    (void)a;\n"
            "}\n"
        )
        out, count, refusals = convert(src)
        self.assertEqual((count, refusals), (1, []))
        self.assertTrue(out.endswith("void g(int) {\n}\n"))

    def test_a_member_of_the_same_name_is_not_a_read(self) -> None:
        src = (
            "bool f(const uint8_t *src, const size_t len) {\n"
            "    DK_NOTUSED(len);\n"
            "    return ((const probe *)src)->len == 0 || s.len;\n"
            "}\n"
        )
        self.assertEqual(
            convert(src)[:2],
            (
                "bool f(const uint8_t *src, const size_t) {\n"
                "    return ((const probe *)src)->len == 0 || s.len;\n"
                "}\n",
                1,
            ),
        )

    def test_a_silencer_of_a_name_the_body_reads_is_redundant(self) -> None:
        src = "void f(it *iter) {\n    iter->valid = 0;\n    (void)iter;\n}\n"
        self.assertEqual(
            convert(src), ("void f(it *iter) {\n    iter->valid = 0;\n}\n", 1, [])
        )

    def test_a_name_read_under_one_branch_only_is_maybe_unused(self) -> None:
        src = (
            "bool f(int sig) {\n"
            "#if L\n"
            "    return g(sig);\n"
            "#else\n"
            "    (void)sig;\n"
            "    return false;\n"
            "#endif\n"
            "}\n"
        )
        self.assertEqual(
            convert(src),
            (
                "bool f([[maybe_unused]] int sig) {\n"
                "#if L\n"
                "    return g(sig);\n"
                "#else\n"
                "    return false;\n"
                "#endif\n"
                "}\n",
                1,
                [],
            ),
        )

    def test_a_certain_read_in_every_build_beats_branch_reads(self) -> None:
        # 'pid <= 0' reads pid in every build; the call under #if does not
        # matter, and the #else keeps its other statements
        src = (
            "int f(pid_t pid) {\n"
            "    if (pid <= 0) {\n"
            "        return -1;\n"
            "    }\n"
            "#if L\n"
            "    return g(pid);\n"
            "#else\n"
            "    (void)pid;\n"
            "    return -1;\n"
            "#endif\n"
            "}\n"
        )
        out, count, _ = convert(src)
        self.assertEqual(count, 1)
        self.assertTrue(out.startswith("int f(pid_t pid) {\n"))
        self.assertIn("#else\n    return -1;\n#endif\n", out)

    def test_a_parameter_only_an_assert_reads_is_maybe_unused(self) -> None:
        src = "void f(int n) {\n    assert(n > 0);\n    (void)n;\n}\n"
        self.assertEqual(
            convert(src),
            ("void f([[maybe_unused]] int n) {\n    assert(n > 0);\n}\n", 1, []),
        )

    def test_an_else_left_empty_goes_with_its_silencer(self) -> None:
        src = "void f(int a) {\n#ifdef X\n    g(a);\n#else\n    (void)a;\n#endif\n}\n"
        self.assertEqual(
            convert(src),
            (
                "void f([[maybe_unused]] int a) {\n#ifdef X\n    g(a);\n#endif\n}\n",
                1,
                [],
            ),
        )

    def test_a_plain_word_in_the_comment_is_prose_not_the_parameter(self) -> None:
        src = (
            "/* Every key hashes alike; a bufferLen of 0 is fine */\n"
            "static uint32_t h(const void *key, size_t bufferLen) {\n"
            "    (void)key;\n"
            "    (void)bufferLen;\n"
            "    return 7;\n"
            "}\n"
        )
        self.assertIn(
            "static uint32_t h(const void *, [[maybe_unused]] size_t bufferLen) {",
            convert(src)[0],
        )

    def test_a_field_or_shadowing_local_of_the_same_name_is_not_a_read(self) -> None:
        src = (
            "int fTest(int argc, char *argv[], int n) {\n"
            "    (void)argc;\n"
            "    (void)n;\n"
            "    typedef struct call { int op; size_t argc; } call;\n"
            "    call c = {.op = 1, .argc = 2};\n"
            "    {\n"
            "        int n = 3;\n"
            "        use(n, c.argc, argv);\n"
            "    }\n"
            "    return 0;\n"
            "}\n"
        )
        out, count, refusals = convert(src)
        self.assertEqual((count, refusals), (2, []))
        self.assertTrue(out.startswith("int fTest(int, char *argv[], int) {\n"))

    def test_a_return_type_on_its_own_line(self) -> None:
        src = (
            "// 'ctx' names the owner\n"
            "static int\n"
            "f(void *ctx) {\n"
            "    (void)ctx;\n"
            "    return 0;\n"
            "}\n"
        )
        self.assertIn("f([[maybe_unused]] void *ctx)", convert(src)[0])


class Locals(unittest.TestCase):
    def test_a_local_only_an_assert_reads_is_maybe_unused(self) -> None:
        src = (
            "void f(void) {\n"
            "    const bool found = g(&v);\n"
            "    assert(found && v);\n"
            "    (void)found;\n"
            "}\n"
        )
        self.assertEqual(
            convert(src),
            (
                "void f(void) {\n"
                "    [[maybe_unused]] const bool found = g(&v);\n"
                "    assert(found && v);\n"
                "}\n",
                1,
                [],
            ),
        )

    def test_a_local_never_used_is_left_for_a_person(self) -> None:
        # often a discarded call, which reads better as (void)g();
        src = "void f(void) {\n    int r = g();\n    (void)r;\n}\n"
        self.assertEqual(convert(src), (src, 0, [unusednames.NEVER_USED]))

    def test_assignments_are_not_reads(self) -> None:
        src = (
            "void f(void) {\n"
            "    size_t next = 0;\n"
            "    for (int i = 0; i < 3; i++) {\n"
            "        next = g(i);\n"
            "        next += 1;\n"
            "        next++;\n"
            "    }\n"
            "    (void)next;\n"
            "}\n"
        )
        out, count, _ = convert(src)
        self.assertEqual(count, 1)
        self.assertIn("    [[maybe_unused]] size_t next = 0;\n", out)
        self.assertNotIn("(void)next", out)

    def test_a_local_read_in_every_build_loses_only_the_silencer(self) -> None:
        src = "void f(void) {\n    int n = g();\n    h(n == 1);\n    (void)n;\n}\n"
        self.assertEqual(
            convert(src),
            ("void f(void) {\n    int n = g();\n    h(n == 1);\n}\n", 1, []),
        )

    def test_a_local_an_assert_and_a_call_use_stays_maybe_unused(self) -> None:
        # the call may be a macro that stores into it (varintSplitFullPut_),
        # so with asserts off it may be set and never read
        src = (
            "#define PUT_(p, w, c) ((w) = put((p), (c)))\n"
            "void f(void) {\n"
            "    varintWidth used;\n"
            "    PUT_(p, used, cols);\n"
            "    assert(used == w);\n"
            "    (void)used;\n"
            "}\n"
        )
        self.assertIn("    [[maybe_unused]] varintWidth used;\n", convert(src)[0])
        # a macro defined in another file counts the same, through 'macros'
        out, _, _ = unusednames.unname(
            src.split("\n", 1)[1].encode(), macros={b"PUT_": frozenset({b"put"})}
        )
        self.assertIn(b"    [[maybe_unused]] varintWidth used;\n", out)

    def test_a_function_argument_is_a_certain_read(self) -> None:
        src = (
            "void f(void) {\n"
            "    flexEntry *fe = NULL;\n"
            "    fe = idx(fl, 3);\n"
            "    assert(get(fe));\n"
            "    show(fe);\n"
            "    (void)fe;\n"
            "}\n"
        )
        self.assertEqual(
            convert(src),
            (
                "void f(void) {\n"
                "    flexEntry *fe = NULL;\n"
                "    fe = idx(fl, 3);\n"
                "    assert(get(fe));\n"
                "    show(fe);\n"
                "}\n",
                1,
                [],
            ),
        )

    def test_test_code_keeps_asserts_so_their_reads_count(self) -> None:
        # ctest.h refuses DATAKIT_TEST with NDEBUG: an assert there always runs
        body = (
            "int fTest(void) {\n"
            "    size_t bytes = 7;\n"
            "    assert(parse(&bytes) && bytes == 32);\n"
            "    (void)bytes;\n"
            "    return 0;\n"
            "}\n"
        )
        plain = "    size_t bytes = 7;\n    assert(parse(&bytes) && bytes == 32);\n"
        out, count, _ = convert(f"#ifdef DATAKIT_TEST\n{body}#endif\n")
        self.assertEqual(count, 1)
        self.assertIn(plain, out)
        out, _, refusals = unusednames.unname(body.encode(), test_file=True)
        self.assertIn(plain, out.decode())
        # the same function in library code: asserts may compile away
        self.assertIn("[[maybe_unused]] size_t bytes", convert(body)[0])
        # and the #else of a test guard is library code again
        out, _, _ = convert(f"#ifdef DATAKIT_TEST\nint x;\n#else\n{body}#endif\n")
        self.assertIn("[[maybe_unused]] size_t bytes", out)

    def test_a_local_counts_reads_in_its_own_block_only(self) -> None:
        # another block's 'deleted' is another variable
        src = (
            "int fTest(void) {\n"
            "    {\n"
            "        const bool deleted = release(a);\n"
            "        (void)deleted;\n"
            "#if 0\n"
            '        printf("%d", deleted);\n'
            "#endif\n"
            "    }\n"
            "    {\n"
            "        const bool deleted = release(b);\n"
            "        show(deleted);\n"
            "    }\n"
            "    return 0;\n"
            "}\n"
        )
        out, count, refusals = convert(src)
        self.assertEqual((count, refusals), (1, []))
        self.assertIn(
            "        [[maybe_unused]] const bool deleted = release(a);\n", out
        )
        self.assertIn("        const bool deleted = release(b);\n", out)

    def test_a_local_read_under_one_branch_is_maybe_unused(self) -> None:
        src = (
            "void f(void) {\n"
            "    int n = g();\n"
            "#ifdef X\n"
            "    h(n);\n"
            "#endif\n"
            "    (void)n;\n"
            "}\n"
        )
        self.assertIn("    [[maybe_unused]] int n = g();\n", convert(src)[0])

    def test_a_comment_or_dead_block_before_the_declaration(self) -> None:
        src = (
            "void f(void) {\n"
            "    assert(a);\n"
            "#if 0\n"
            "    show(a);\n"
            "#endif\n"
            "    /* estimate */ volatile uint64_t dummy = 0;\n"
            "    dummy += 1;\n"
            "    (void)dummy;\n"
            "}\n"
        )
        self.assertIn(
            "    /* estimate */ [[maybe_unused]] volatile uint64_t dummy = 0;\n",
            convert(src)[0],
        )

    def test_a_local_already_marked_loses_only_the_silencer(self) -> None:
        src = "void f(void) {\n    [[maybe_unused]] int r = g();\n    (void)r;\n}\n"
        self.assertEqual(
            convert(src)[:2],
            ("void f(void) {\n    [[maybe_unused]] int r = g();\n}\n", 1),
        )

    def test_a_pointer_local_in_a_nested_block(self) -> None:
        src = (
            "void f(void) {\n"
            "    if (a) {\n"
            "        multilruPtr *p1 = g();\n"
            "        assert(p1 != 0);\n"
            "        (void)p1;\n"
            "    }\n"
            "}\n"
        )
        self.assertIn(
            "        [[maybe_unused]] multilruPtr *p1 = g();\n", convert(src)[0]
        )


class Speed(unittest.TestCase):
    def test_a_long_statement_before_a_silencer_stays_linear(self) -> None:
        # words then a '-': not a declaration, found without backtracking
        words = " ".join(f"identifierNumber{i}" for i in range(60))
        src = (
            "void f(void) {\n"
            "    int r = g();\n"
            f"    M({words} - r);\n"
            f"    {words} - r;\n"
            "    (void)r;\n"
            "}\n"
        )
        self.assertEqual(convert(src)[1], 1)


class Refuses(unittest.TestCase):
    def assertRefused(self, src: str, reason: str) -> None:
        out, count, refusals = convert(src)
        self.assertEqual((out, count), (src, 0))
        self.assertEqual(refusals, [reason])

    def test_a_local_declared_with_other_names(self) -> None:
        self.assertRefused(
            "void f(void) {\n    int a = 1, b = g(a);\n    (void)b;\n}\n",
            unusednames.SHARED_DECLARATION,
        )

    def test_a_name_with_no_declaration_here(self) -> None:
        self.assertRefused(
            "void f(void) {\n    (void)global;\n}\n", unusednames.NO_DECLARATION
        )

    def test_a_preprocessor_line_in_the_body_that_names_it(self) -> None:
        self.assertRefused(
            "void f(int a) {\n    (void)a;\n#define LOCAL(x) ((x) + a)\n    g();\n}\n",
            unusednames.DIRECTIVE,
        )

    def test_a_comment_on_a_preprocessor_line_is_not_a_use(self) -> None:
        src = "void f(int a) {\n    (void)a;\n#if L /* uses a */\n    g();\n#endif\n}\n"
        out, count, refusals = convert(src)
        self.assertEqual((count, refusals), (1, []))
        self.assertTrue(out.startswith("void f(int) {\n#if L"))

    def test_a_silencer_that_is_not_a_statement_of_its_own(self) -> None:
        self.assertRefused(
            "void f(int a) {\n    if (g()) (void)a;\n    h();\n}\n",
            unusednames.NOT_A_STATEMENT,
        )

    def test_a_function_pointer_parameter(self) -> None:
        self.assertRefused(
            "void f(void (*cb)(int)) {\n    (void)cb;\n}\n",
            unusednames.NO_DECLARATION,
        )

    def test_a_name_a_called_macro_uses_by_itself(self) -> None:
        # ERR (ctest.h) ends with err++ on whatever 'err' is in scope
        src = (
            "#define ERR(x, ...) do { report(x); err++; } while (0)\n"
            "static void miss(const char *n, int *err) {\n"
            "    (void)err;\n"
            '    ERR("%s", n);\n'
            "}\n"
        )
        self.assertRefused(src, unusednames.IMPLICIT)
        # the same through a header's macro
        body = src.split("\n", 1)[1]
        out, count, refusals = unusednames.unname(
            body.encode(), macros={b"ERR": frozenset({b"report", b"err", b"x"})}
        )
        self.assertEqual(
            (count, [r.reason for r in refusals]), (0, [unusednames.IMPLICIT])
        )

    def test_words_in_a_macro_string_or_comment_are_not_names(self) -> None:
        src = (
            '#define MISS(k) report("key " #k " not found") /* found */\n'
            "static void f(int found) {\n"
            "    (void)found;\n"
            "    MISS(x);\n"
            "}\n"
        )
        self.assertIn("static void f(int) {", convert(src)[0])

    def test_the_refusal_names_the_macro(self) -> None:
        src = (
            "#define BUMP() (err++)\n"
            "#define FAIL() BUMP()\n"
            "static void f(int *err) {\n"
            "    (void)err;\n"
            "    FAIL();\n"
            "}\n"
        )
        _, _, refusals = unusednames.unname(src.encode())
        self.assertEqual(refusals[0].detail, "via FAIL")

    def test_a_macro_parameter_of_the_same_name_is_not_implicit(self) -> None:
        src = (
            "#define SHOW(err) report(err)\n"
            "static void f(int err, int v) {\n"
            "    (void)err;\n"
            "    SHOW(v);\n"
            "}\n"
        )
        self.assertIn("static void f(int, int v) {", convert(src)[0])

    def test_a_macro_body_is_left_alone(self) -> None:
        src = "#define SILENCE(x) (void)x;\nint v;\n"
        self.assertEqual(convert(src), (src, 0, []))

    def test_initializers_and_types_are_not_functions(self) -> None:
        src = "struct s { int a; };\nstatic const int t[] = {1, 2};\n"
        self.assertEqual(convert(src), (src, 0, []))

    def test_literals_and_comments_are_not_silencers(self) -> None:
        src = 'void f(int a) {\n    puts("(void)a;"); /* (void)a; */\n    g(a);\n}\n'
        self.assertEqual(convert(src), (src, 0, []))


class Files(unittest.TestCase):
    def test_dry_run_reports_and_changes_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "src").mkdir()
            path = repo / "src" / "a.c"
            src = (
                b"void f(int a) {\n    (void)a;\n}\n"
                b"void g(void (*cb)(int)) {\n    int r;\n    (void)cb;\n}\n"
            )
            path.write_bytes(src)
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                self.assertEqual(unusednames.run(repo, ("src/a.c",), True), 0)
            self.assertEqual(path.read_bytes(), src)
            text = buf.getvalue()
            self.assertIn("src/a.c: 1", text)
            self.assertIn("src/a.c:6: cb: " + unusednames.NO_DECLARATION, text)
            with contextlib.redirect_stdout(io.StringIO()):
                unusednames.run(repo, ("src/a.c",), False)
            self.assertTrue(path.read_bytes().startswith(b"void f(int) {\n}\n"))

    def test_macros_from_vendored_headers_count(self) -> None:
        # varintSplitFullGet_ (deps/varint) stores into its second argument
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "src").mkdir()
            (repo / "deps" / "v").mkdir(parents=True)
            (repo / "deps" / "v" / "v.h").write_bytes(
                b"#define GET_(src, len, val) ((len) = get((src), &(val)))\n"
            )
            path = repo / "src" / "a.c"
            path.write_bytes(
                b"void f(void) {\n    int w;\n    GET_(p, w, x);\n    (void)w;\n}\n"
            )
            with contextlib.redirect_stdout(io.StringIO()):
                unusednames.run(repo, ("src/a.c",), False)
            self.assertIn(b"    [[maybe_unused]] int w;\n", path.read_bytes())

    def test_a_missing_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp, self.assertRaises(ToolError):
            unusednames.run(Path(tmp), ("src/missing.c",), True)


if __name__ == "__main__":
    unittest.main()
