"""``dktools wrapcalls``: which reported calls are wrapped, and how.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from dktools import wrapcalls

MACRO = "OK"


def wrap(src: str, line: int, col: int) -> tuple[str, list[wrapcalls.Skip]]:
    site = wrapcalls.Site("f.c", line, col)
    out, _, skipped = wrapcalls.wrap_file(src.encode(), [site], MACRO)
    return out.decode(), skipped


def warning(path: str, line: int, col: int, type_name: str = "res") -> str:
    return (
        f"{path}:{line}:{col}: warning: ignoring return value of type "
        f"'{type_name}' declared with 'nodiscard' attribute [-Wunused-value]"
    )


class Wraps(unittest.TestCase):
    def test_a_statement_call_is_wrapped(self) -> None:
        out, skipped = wrap("void f(void) {\n    g(a, b);\n}\n", 2, 5)
        self.assertEqual(out, "void f(void) {\n    OK(g(a, b));\n}\n")
        self.assertEqual(skipped, [])

    def test_a_call_over_several_lines_is_wrapped_whole(self) -> None:
        src = "{\n    g(a,\n      h(b));\n}\n"
        out, skipped = wrap(src, 2, 5)
        self.assertEqual(out, "{\n    OK(g(a,\n      h(b)));\n}\n")
        self.assertEqual(skipped, [])

    def test_parentheses_in_literals_and_comments_do_not_count(self) -> None:
        src = "{\n    g(\")\", ')', /* ) */ x);\n}\n"
        out, _ = wrap(src, 2, 5)
        self.assertEqual(out, "{\n    OK(g(\")\", ')', /* ) */ x));\n}\n")

    def test_a_statement_after_a_comment_or_directive_is_wrapped(self) -> None:
        src = "{\n    a(); // note (\n#endif\n    g(x);\n}\n"
        out, skipped = wrap(src, 4, 5)
        self.assertIn("    OK(g(x));", out)
        self.assertEqual(skipped, [])

    def test_several_sites_in_one_file_all_land(self) -> None:
        src = "{\n    g(1);\n    g(2); h(3);\n}\n"
        sites = [wrapcalls.Site("f.c", 2, 5), wrapcalls.Site("f.c", 3, 5)]
        out, done, skipped = wrapcalls.wrap_file(src.encode(), sites, MACRO)
        self.assertEqual(out.decode(), "{\n    OK(g(1));\n    OK(g(2)); h(3);\n}\n")
        self.assertEqual((len(done), skipped), (2, []))


class Skips(unittest.TestCase):
    def reason(self, src: str, line: int, col: int) -> str:
        out, skipped = wrap(src, line, col)
        self.assertEqual(out, src, "a skipped site must not change the file")
        self.assertEqual(len(skipped), 1)
        return skipped[0].reason

    def test_a_call_inside_an_assignment_on_its_line(self) -> None:
        self.assertEqual(
            self.reason("{\n    x = g(a);\n}\n", 2, 9), "not at the start of its line"
        )

    def test_an_assignment_split_over_lines(self) -> None:
        self.assertEqual(
            self.reason("{\n    int x =\n        g(a);\n}\n", 3, 9),
            "continues an expression",
        )

    def test_a_call_inside_a_larger_expression(self) -> None:
        self.assertEqual(
            self.reason("{\n    g(a) || h(b);\n}\n", 2, 5), "not a whole statement"
        )

    def test_a_column_with_no_call(self) -> None:
        self.assertEqual(
            self.reason("{\n    (g)(a);\n}\n", 2, 5), "no call at the reported column"
        )


class Logs(unittest.TestCase):
    def test_scratch_paths_map_back_to_the_repository(self) -> None:
        repo = Path("/r")
        logged = "/r/build/dktools/verify/x/src/src/../src/a.c"
        self.assertEqual(wrapcalls.repo_path(logged, repo), "src/a.c")
        self.assertIsNone(wrapcalls.repo_path("/elsewhere/a.c", repo))

    def test_only_the_named_type_and_each_site_once(self) -> None:
        repo = Path("/r")
        lines = [
            warning("/r/build/dktools/verify/x/src/src/a.c", 3, 5),
            warning("/r/build/dktools/verify/x/src/src/a.c", 3, 5),
            warning("/r/build/dktools/verify/x/src/src/a.c", 9, 5, "other"),
            "noise",
        ]
        self.assertEqual(
            wrapcalls.sites(lines, "res", repo), [wrapcalls.Site("src/a.c", 3, 5)]
        )

    def test_run_writes_files_and_exits_1_on_a_skip(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "src").mkdir()
            source = repo / "src" / "a.c"
            source.write_text("{\n    g(1);\n    x = g(2);\n}\n")
            log = repo / "build.log"
            scratch = f"{tmp}/build/dktools/verify/v/src/src/a.c"
            log.write_text(warning(scratch, 2, 5) + "\n" + warning(scratch, 3, 9))
            with contextlib.redirect_stdout(io.StringIO()) as out:
                dry = wrapcalls.run(repo, log, "res", MACRO, dry_run=True)
            self.assertEqual(dry, 1)
            self.assertIn("would wrap 1 of 2", out.getvalue())
            self.assertIn("g(1);", source.read_text())
            with contextlib.redirect_stdout(io.StringIO()):
                code = wrapcalls.run(repo, log, "res", MACRO, dry_run=False)
            self.assertEqual(code, 1)
            self.assertEqual(source.read_text(), "{\n    OK(g(1));\n    x = g(2);\n}\n")

    def test_path_limits_the_files_touched(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "src").mkdir()
            for name in ("a.c", "b.c"):
                (repo / "src" / name).write_text("{\n    g(1);\n}\n")
            log = repo / "build.log"
            scratch = f"{tmp}/build/dktools/verify/v/src/src"
            log.write_text(
                warning(f"{scratch}/a.c", 2, 5) + "\n" + warning(f"{scratch}/b.c", 2, 5)
            )
            with contextlib.redirect_stdout(io.StringIO()):
                code = wrapcalls.run(repo, log, "res", MACRO, False, ("src/b.c",))
            self.assertEqual(code, 0)
            self.assertEqual((repo / "src" / "a.c").read_text(), "{\n    g(1);\n}\n")
            self.assertEqual(
                (repo / "src" / "b.c").read_text(), "{\n    OK(g(1));\n}\n"
            )


if __name__ == "__main__":
    unittest.main()
