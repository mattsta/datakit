"""``dktools testchecks``: which TEST blocks count as checking nothing.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from dktools import testchecks
from dktools.gitutil import ToolError


def names_in(src: bytes, ctest: bytes = b"") -> list[str]:
    return [h.name for h in testchecks.scan("src/x.c", src, ctest)]


class Scan(unittest.TestCase):
    def test_a_block_without_a_check_is_listed(self) -> None:
        src = b'TEST("frees") {\n    thingFree(thingNew());\n}\n'
        self.assertEqual(names_in(src), ['"frees"'])
        self.assertEqual(testchecks.scan("src/x.c", src)[0].line, 1)

    def test_each_check_counts(self) -> None:
        for check in (
            b'ERR("bad");',
            b"ERROR;",
            b"TEST_BYTES(err, a, b, 4);",
            b"TEST_HEX(err, h, o, 4);",
            b"assert(x);",
            b"err += helperTest();",
        ):
            with self.subTest(check=check):
                src = b'TEST("t") {\n    ' + check + b"\n}\n"
                self.assertEqual(names_in(src), [])

    def test_a_check_macro_counts_through_other_macros(self) -> None:
        src = (
            b'#define CHECK_OK(x) do { if (!(x)) { ERR("%s", #x); } } while (0)\n'
            b"#define ROW_OK(r) CHECK_OK(rowValid(r))\n"
            b'TEST("rows") {\n    ROW_OK(row);\n}\n'
        )
        self.assertEqual(names_in(src), [])

    def test_ctest_macros_count(self) -> None:
        ctest = b'#define TEST_NONZERO(x) if (!(x)) { ERR("zero"); }\n'
        src = b'TEST("t") {\n    TEST_NONZERO(n);\n}\n'
        self.assertEqual(names_in(src, ctest), [])
        self.assertEqual(names_in(src), ['"t"'])

    def test_comments_literals_and_other_names_do_not_count(self) -> None:
        for body in (
            b"/* ERR if this breaks */ f();",
            b'printf("assert nothing");',
            b"errno = 0; nerr++;",
        ):
            with self.subTest(body=body):
                src = b'TEST("t") {\n    ' + body + b"\n}\n"
                self.assertEqual(names_in(src), ['"t"'])

    def test_timing_blocks_and_mentions_are_skipped(self) -> None:
        for src in (
            b'TEST("PERF: inserts") {\n    run();\n}\n',
            b'TEST("encode performance (scalar)") {\n    run();\n}\n',
            b'TEST("Speed of printing") {\n    run();\n}\n',
            b'TEST("convert") {\n    TIME_INIT;\n    run();\n'
            b'    TIME_FINISH(n, "convert");\n}\n',
            b"#define TEST(name) printf(name);\n",
            b'void f(void) { TEST("x"); }\n',
        ):
            with self.subTest(src=src):
                self.assertEqual(names_in(src), [])

    def test_a_multi_line_name_is_one_line_and_nesting_is_matched(self) -> None:
        src = (
            b'TEST_DESC("fill %zu",\n          f) {\n'
            b"    if (a) { if (b) { g(); } }\n}\n"
            b'TEST("next") {\n    assert(1);\n}\n'
        )
        self.assertEqual(names_in(src), ['"fill %zu", f'])


class Run(unittest.TestCase):
    def test_counts_and_exit_codes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "src").mkdir()
            (repo / "src/a.c").write_bytes(b'TEST("t") {\n    f();\n}\n')
            (repo / "src/b.c").write_bytes(b'TEST("t") {\n    assert(1);\n}\n')
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(testchecks.run(repo, ()), 1)
                self.assertEqual(testchecks.run(repo, ("src/b.c",)), 0)
            text = out.getvalue()
            self.assertIn('src/a.c:1: TEST "t": checks nothing', text)
            self.assertIn("1 blocks with no check, 0 allowed", text)
            with self.assertRaises(ToolError):
                testchecks.run(repo, ("src/missing.c",))

    def test_a_check_macro_in_a_header_counts(self) -> None:
        # A module's header defines its tests' check macro; scanning only
        # the .c file still sees it
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "src").mkdir()
            (repo / "src/m.h").write_bytes(
                b'#define M_OK(c) do { if (!(c)) { ERR("%s", #c); } } while (0)\n'
            )
            (repo / "src/m.c").write_bytes(b'TEST("t") {\n    M_OK(f());\n}\n')
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(testchecks.run(repo, ("src/m.c",)), 0)

    def test_allow_lines_excuse_named_blocks_and_go_stale(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "src").mkdir()
            (repo / "tools").mkdir()
            (repo / "src/a.c").write_bytes(
                b'TEST("prints") {\n    repr();\n}\n'
                b'TEST_DESC("frees %d", n) {\n    f(NULL);\n}\n'
            )
            allow = repo / "tools/testchecks-allow.txt"
            allow.write_text(
                "# comment\n"
                'src/a.c :: "prints" :: a printer; running is the contract\n'
                'src/a.c :: "frees %d", n :: free(NULL) is the contract\n'
            )
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(testchecks.run(repo, ()), 0)
            self.assertIn("0 blocks with no check, 2 allowed", out.getvalue())

            # A block that gains a check leaves its allow line stale
            (repo / "src/a.c").write_bytes(
                b'TEST("prints") {\n    assert(repr());\n}\n'
                b'TEST_DESC("frees %d", n) {\n    f(NULL);\n}\n'
            )
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(testchecks.run(repo, ()), 1)
            self.assertIn('"prints": allowed but no longer listed', out.getvalue())

            for bad in ("src/a.c :: name\n", 'src/a.c :: "x" ::  \n'):
                with self.subTest(bad=bad):
                    allow.write_text(bad)
                    with self.assertRaises(ToolError):
                        testchecks.run(repo, ())

    def test_an_empty_tree_is_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "src").mkdir()
            with self.assertRaises(ToolError):
                testchecks.run(Path(tmp), ())


if __name__ == "__main__":
    unittest.main()
