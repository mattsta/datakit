"""``dktools idioms``: which hand-written forms each rule finds.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from dktools import idioms
from dktools.gitutil import ToolError


def rules_in(src: bytes, path: str = "src/x.c") -> list[str]:
    return [h.rule for h in idioms.scan(path, src)]


class Rules(unittest.TestCase):
    def test_each_rule_finds_its_idiom(self) -> None:
        cases = {
            "byte-shift-loop": b"v |= (uint64_t)b[i] << (8 * i);\n",
            "inline-lcg": b"s = s * 6364136223846793005ULL + 1;\n",
            "inline-xorshift64star": b"return x * 0x2545F4914F6CDD1Dull;\n",
            "rand-64": b"uint64_t v = ((uint64_t)rand() << 32) | rand();\n",
            "qsort-comparator": (
                b"static int cmp(const void *a, const void *b) {\n"
                b"    const uint64_t x = *(const uint64_t *)a;\n"
                b"    const uint64_t y = *(const uint64_t *)b;\n"
                b"    return (x > y) - (x < y);\n}\n"
            ),
            "div-ceil": b"#define D(a, b) (((a) + (b) - 1) / (b))\n",
            "encode-op-boxes": b"flex *f = flexNewFromDataboxPtrsExact(boxes, argc);\n",
            "memcmp-err": (b'if (memcmp(a, b, 4) != 0) {\n    ERR("x");\n}\n'),
            "databox-int-cast": b'ERR("%" PRId64, (int64_t)got.box.data.i);\n',
        }
        for rule, src in cases.items():
            with self.subTest(rule=rule):
                self.assertEqual(rules_in(src), [rule])

    def test_look_alikes_do_not_match(self) -> None:
        for src in (
            b"x = (1u << (8 * sizeof(T))) - 1;\n",  # not a byte index
            b"/* v |= b[i] << (8 * i) */\n",  # a comment
            b'const char *s = "rand() << 32";\n',  # a literal
            b"y = (n + 7) / 8;\n",  # a constant divisor, not (a + b - 1) / b
            # a row of a known width, not an encodeOp's operands
            b"flex *f = flexNewFromDataboxPtrsExact(row, 2);\n",
            b"static int cmp(const void *a, const void *b) {\n"
            b"    const row *x = a;\n    return 0;\n}\n",  # a struct comparator
            # ordering by what the loaded indices point at
            b"static int cmp(const void *a, const void *b) {\n"
            b"    const uint32_t ia = *(const uint32_t *)a;\n"
            b"    const uint32_t ib = *(const uint32_t *)b;\n"
            b"    const int64_t sa = s[ia];\n    return 0;\n}\n",
            # a cast across signs converts the value
            b"m = negative ? 0 - (uint64_t)box->data.i : box->data.u;\n",
            b"v = (int64_t)entry.box.data.u;\n",
        ):
            with self.subTest(src=src):
                self.assertEqual(rules_in(src), [])

    def test_a_databox_cast_matches_each_access_form(self) -> None:
        for src in (
            b"f((uint64_t)ids[i].data.u);\n",
            b"f((int64_t) box->data.i);\n",
            b"f((uint64_t)key.data.u, want);\n",
        ):
            with self.subTest(src=src):
                self.assertEqual(rules_in(src), ["databox-int-cast"])

    def test_a_rules_home_file_is_skipped(self) -> None:
        src = b"return x * 0x2545F4914F6CDD1Dull;\n"
        self.assertEqual(rules_in(src, "src/dkRandom.h"), [])

    def test_hits_carry_their_line(self) -> None:
        hits = idioms.scan("src/x.c", b"a;\nb;\ns = s * 6364136223846793005;\n")
        self.assertEqual([(h.line, h.rule) for h in hits], [(3, "inline-lcg")])


class Allow(unittest.TestCase):
    def test_allow_lines_need_a_known_rule_and_a_reason(self) -> None:
        self.assertEqual(
            idioms.parse_allow("# note\nsrc/a.c inline-lcg pinned stream\n"),
            {("src/a.c", "inline-lcg"): "pinned stream"},
        )
        for bad in ("src/a.c inline-lcg\n", "src/a.c no-such-rule why\n"):
            with self.subTest(bad=bad), self.assertRaises(ToolError):
                idioms.parse_allow(bad)

    def run_in(self, files: dict[str, bytes], allow: str) -> tuple[int, str]:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "tools").mkdir()
            (repo / "tools" / "idioms-allow.txt").write_text(allow)
            for name, body in files.items():
                (repo / name).write_bytes(body)
            with contextlib.redirect_stdout(io.StringIO()) as out:
                code = idioms.run(repo, tuple(files))
            return code, out.getvalue()

    def test_an_exported_tree_is_walked_and_an_empty_one_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "tools").mkdir()
            (repo / "tools" / "idioms-allow.txt").write_text("")
            with self.assertRaises(ToolError):
                idioms.run(repo, ())
            (repo / "src" / "sub").mkdir(parents=True)
            (repo / "src" / "sub" / "a.c").write_bytes(
                b"s = s * 6364136223846793005;\n"
            )
            with contextlib.redirect_stdout(io.StringIO()) as out:
                self.assertEqual(idioms.run(repo, ()), 1)
            self.assertIn("src/sub/a.c:1: inline-lcg", out.getvalue())

    def test_an_allowed_hit_passes_and_a_stale_line_fails(self) -> None:
        lcg = b"s = s * 6364136223846793005;\n"
        code, out = self.run_in({"a.c": lcg}, "a.c inline-lcg pinned\n")
        self.assertEqual(code, 0, out)
        code, out = self.run_in({"a.c": b"x;\n"}, "a.c inline-lcg pinned\n")
        self.assertEqual(code, 1)
        self.assertIn("no longer found", out)
        code, out = self.run_in({"a.c": lcg}, "")
        self.assertEqual(code, 1)
        self.assertIn("a.c:1: inline-lcg", out)


if __name__ == "__main__":
    unittest.main()
