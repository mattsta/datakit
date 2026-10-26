"""``dktools allocaudit``: the process pool end to end.

Real libclang runs in spawn-started workers over a throwaway tree; a
worker that dies or hangs is a one-line exit-2 error, never a hang.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock

import prochelpers
from dktools import allocaudit, cli

ANNOTATED = """\
void *make(void) {
    return zmalloc(8); /* @own: caller */
}
"""

VIOLATION = """\
void leak(void) {
    char *p = zmalloc(8);
    (void)p;
}
"""

#: A bigger file, so largest-first scheduling hands it out first.
SCOPED = (
    "void scoped(void) {\n"
    "    cleaner(zfree) void *x = zmalloc(8);\n"
    "    (void)x;\n"
    "}\n" + "/* padding */\n" * 200
)


class AllocAudit(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        self.write("src/a.c", ANNOTATED)
        self.write("src/b.c", SCOPED)

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def write(self, rel: str, text: str) -> None:
        path = self.repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)

    def cli(self, *args: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = cli.main(["--repo", str(self.repo), "allocaudit", *args])
        return code, out.getvalue(), err.getvalue()

    def test_clean_tree_in_file_order(self) -> None:
        timings: list[tuple[str, float]] = []
        result = allocaudit.run_audit(self.repo, jobs=2, timings=timings)
        self.assertEqual(
            [(s.path, s.status) for s in result.sites],
            [("src/a.c", "ANNOTATED"), ("src/b.c", "SCOPED")],
        )
        self.assertEqual([path for path, _ in timings], ["src/a.c", "src/b.c"])
        code, out, _ = self.cli("check", "-j", "2")
        self.assertEqual(code, 0, out)
        self.assertIn("100% clean pattern", out)

    def test_violation_exits_1(self) -> None:
        self.write("src/c.c", VIOLATION)
        code, out, _ = self.cli("check", "-j", "2")
        self.assertEqual(code, 1, out)
        self.assertIn("VIOLATION src/c.c:2 in leak", out)

    def test_a_libc_allocator_call_is_a_violation(self) -> None:
        self.write(
            "src/c.c",
            "void t(void) {\n    unsigned *a = malloc(8);\n    free(a);\n}\n",
        )
        result = allocaudit.run_audit(self.repo, jobs=2)
        libc = [
            (s.line, s.allocator, s.status) for s in result.sites if s.path == "src/c.c"
        ]
        self.assertEqual(libc, [(2, "malloc", "VIOLATION"), (3, "free", "VIOLATION")])
        code, out, _ = self.cli("check", "-j", "2")
        self.assertEqual(code, 1, out)

    def test_an_explicit_opt_out_a_field_or_a_declaration_is_not(self) -> None:
        self.write(
            "src/c.c",
            "#define IKNOWWHATIMDOING /* a libc baseline on purpose */\n"
            "void t(void) { void *p = malloc(8); free(p); }\n",
        )
        self.write(
            "src/d.c",
            "struct s { void (*free)(void *); };\n"
            "void *calloc(unsigned long n, unsigned long size);\n"
            "void u(struct s *o, struct s v, void *p) { o->free(p); v.free(p); }\n",
        )
        result = allocaudit.run_audit(self.repo, jobs=2)
        self.assertEqual(
            [s for s in result.sites if s.path in ("src/c.c", "src/d.c")], []
        )

    def test_killed_worker_exits_2_without_hanging(self) -> None:
        start = time.monotonic()
        with mock.patch.object(allocaudit, "_audit_one", prochelpers.die_audit):
            code, _, err = self.cli("check", "-j", "2")
        self.assertEqual(code, 2)
        self.assertRegex(err, r"^dktools allocaudit: a worker process died .*src/")
        self.assertEqual(len(err.splitlines()), 1, err)
        self.assertLess(time.monotonic() - start, 30)

    def test_stuck_worker_exits_2_at_the_timeout(self) -> None:
        start = time.monotonic()
        with mock.patch.object(allocaudit, "_audit_one", prochelpers.hang_audit):
            code, _, err = self.cli("check", "-j", "2", "--timeout", "1")
        self.assertEqual(code, 2)
        self.assertRegex(
            err, r"^dktools allocaudit: no file finished in 1s, 2 left \(in flight: "
        )
        self.assertLess(time.monotonic() - start, 30)
        pids = prochelpers.read_pids(self.repo)
        self.assertTrue(pids)
        self.assertEqual(prochelpers.wait_gone(pids), [])

    def test_bad_arguments_are_input_errors(self) -> None:
        for args in (("-j", "0"), ("--timeout", "-1")):
            with self.subTest(args=args):
                code, _, err = self.cli("check", *args)
                self.assertEqual(code, 2)
                self.assertEqual(len(err.splitlines()), 1, err)


if __name__ == "__main__":
    unittest.main()
