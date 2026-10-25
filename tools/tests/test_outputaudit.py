"""``dktools outputaudit``: what counts as library output, and its sorting.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from dktools import cli, outputaudit


def scan(src: str, path: str = "src/x.c") -> list[tuple[str, str, str]]:
    return [(s.call, s.context, s.category) for s in outputaudit.scan_text(path, src)]


class Discovery(unittest.TestCase):
    def test_standard_stream_writes_are_found(self) -> None:
        src = """
        int f(FILE *fp, int fd) {
            printf("a");
            fprintf(stderr, "b");
            fputs("c", stdout);
            fwrite("d", 1, 1, stderr);
            perror("e");
            write(2, "f", 1);
            dprintf(STDOUT_FILENO, "g");
            fprintf(fp, "caller stream");
            fwrite("h", 1, 1, fp);
            write(fd, "i", 1);
            fflush(stdout);
            return 0;
        }
        """
        self.assertEqual(
            [c for c, _, _ in scan(src)],
            ["printf", "fprintf", "fputs", "fwrite", "perror", "write", "dprintf"],
        )

    def test_comments_strings_and_members_are_not_calls(self) -> None:
        src = """
        void f(struct ops *o) {
            /* printf("in a comment"); */
            const char *s = "printf(\\"in a string\\")";
            o->printf("member");
            o.write(1, s);
        }
        """
        self.assertEqual(scan(src), [])

    def test_test_code_is_skipped(self) -> None:
        src = """
        void f(void) { printf("library"); }
        #ifdef DATAKIT_TEST
        int fTest(void) { printf("test"); return 0; }
        #endif
        """
        self.assertEqual(scan(src), [("printf", "f", "unrequested")])


class Sorting(unittest.TestCase):
    def test_repr_and_print_functions_are_requested(self) -> None:
        src = """
        void flexRepr(const flex *f) { printf("x"); }
        void multidictPrintStats(void) { printf("y"); }
        int flexAdd(int a) {
            if (a < 0) {
                fprintf(stderr, "bad");
            }
            return a;
        }
        """
        self.assertEqual(
            scan(src),
            [
                ("printf", "flexRepr", "requested"),
                ("printf", "multidictPrintStats", "requested"),
                ("fprintf", "flexAdd", "unrequested"),
            ],
        )

    def test_macros_are_listed_at_the_definition(self) -> None:
        src = """
        #define WARN(msg) do { fprintf(stderr, "%s", msg); } while (0)
        #define REPR_LINE(x) \\
            printf("%d", x)
        int g(void) { return 1; }
        """
        self.assertEqual(
            scan(src),
            [
                ("fprintf", "#define WARN", "unrequested"),
                ("printf", "#define REPR_LINE", "requested"),
            ],
        )

    def test_dead_and_debug_code(self) -> None:
        src = """
        #define NOISY 0
        int f(int x) {
        #if 0
            printf("dead");
        #endif
        #if NOISY
            printf("noisy");
        #else
            fprintf(stderr, "live else branch");
        #endif
        #ifdef DEBUG_RESIZE
            printf("debug");
        #endif
        #ifndef NDEBUG
            printf("assert builds");
        #endif
        #if !DEBUG_OFF
            printf("negated switch is live");
        #endif
            return x;
        }
        """
        self.assertEqual(
            [cat for _, _, cat in scan(src)],
            ["dead", "debug", "unrequested", "debug", "debug", "unrequested"],
        )

    def test_operator_tools(self) -> None:
        src = 'void report(void) { printf("MB/s"); }\n'
        for path in ("src/dataspeed.c", "src/memtest.c"):
            with self.subTest(path=path):
                self.assertEqual(scan(src, path), [("printf", "report", "tool")])
        self.assertEqual(
            scan(src, "src/dataspeedy.c"), [("printf", "report", "unrequested")]
        )

    def test_test_only_files(self) -> None:
        src = 'void bench(void) { printf("t"); }\n'
        for path in ("src/intsetTest.c", "src/memboundBench.c", "src/datakit-test.c"):
            with self.subTest(path=path):
                self.assertEqual(scan(src, path), [("printf", "bench", "test")])


class Cli(unittest.TestCase):
    def test_check_fails_on_unrequested_output(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "src").mkdir()
            (repo / "src" / "datakit.h").write_text("")
            (repo / "src" / "a.c").write_text('void aRepr(void) { printf("r"); }\n')
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.assertEqual(cli.main(["--repo", tmp, "outputaudit", "check"]), 0)
            (repo / "src" / "b.c").write_text(
                'int b(void) { puts("oops"); return 0; }\n'
            )
            with contextlib.redirect_stdout(out):
                self.assertEqual(cli.main(["--repo", tmp, "outputaudit", "check"]), 1)
            self.assertIn("UNREQUESTED src/b.c:1: puts to stdout in b", out.getvalue())


if __name__ == "__main__":
    unittest.main()
