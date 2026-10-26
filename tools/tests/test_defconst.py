"""``dktools defconst``: which #define numbers become static constexpr.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from dktools import defconst


class Types(unittest.TestCase):
    def test_the_literal_keeps_its_type(self) -> None:
        for literal, ctype in (
            ("64", "int"),
            ("-1", "int"),
            ("1'000'000", "int"),
            ("0x7fffffff", "int"),
            ("0x80000000", "unsigned"),
            ("0x3320646eu", "unsigned"),
            ("20000u", "unsigned"),
            ("4294967296", "int64_t"),
            ("1L", "int64_t"),
            ("1'000'000'000ULL", "uint64_t"),
            ("3UL", "uint64_t"),
            ("(1024)", "int"),
        ):
            with self.subTest(literal=literal):
                self.assertEqual(defconst.literal_type(literal), ctype)

    def test_not_an_integer_literal(self) -> None:
        for literal in ("1.5", "1e9", "'a'", "x", "1 << 4"):
            with self.subTest(literal=literal):
                self.assertIsNone(defconst.literal_type(literal))


class Classify(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.repo = Path(self._tmp.name)
        (self.repo / "src").mkdir()

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def write(self, rel: str, text: str) -> None:
        (self.repo / rel).write_text(text)

    def plan(self) -> dict[str, str]:
        return {
            c.name: c.verdict
            for c in defconst.classify(self.repo, defconst.tree_files(self.repo))
        }

    def test_verdicts(self) -> None:
        self.write(
            "src/a.c",
            "#define SMALL_MAX 64\n"
            "#define USE_NEON 1\n"
            "#if USE_NEON\n#endif\n"
            "#ifndef TUNABLE\n#define TUNABLE 8\n#endif\n"
            '#define TPARAM 0\n#include "impl.h"\n#undef TPARAM\n'
            "#define NAMED 3\n#define STR_(x) #x\n#define S STR_(NAMED)\n"
            "#define TWICE 1\n",
        )
        self.write(
            "src/b.c",
            "#define TWICE 2\n#define LOCAL 5u\n"
            "int g(void) { return SMALL_MAX + TWICE + LOCAL + NAMED + INNER; }\n",
        )
        self.write("src/pub.h", "#define PUB_LEN 25\n")
        self.write("src/thingInternal.h", "#define INNER 7\n")
        self.assertEqual(
            self.plan(),
            {
                "SMALL_MAX": defconst.CONVERT,
                "USE_NEON": defconst.PREPROCESSOR,
                "TUNABLE": defconst.PREPROCESSOR,
                "TPARAM": defconst.PREPROCESSOR,
                "NAMED": defconst.CONVERT,
                "TWICE": defconst.MULTIPLE,
                "LOCAL": defconst.CONVERT,
                "PUB_LEN": defconst.PUBLIC,
                "INNER": defconst.CONVERT,
            },
        )

    def test_a_vendored_header_reading_the_name_keeps_it_a_macro(self) -> None:
        (self.repo / "deps" / "v").mkdir(parents=True)
        (self.repo / "deps" / "v" / "packed.h").write_text(
            "#if PACK_BITS > 64\n#error too wide\n#endif\n"
        )
        self.write("src/a.c", '#define PACK_BITS 12\n#include "packed.h"\n')
        self.assertEqual(self.plan()["PACK_BITS"], defconst.PREPROCESSOR)

    def test_a_name_another_read_macro_expands_to_is_read_too(self) -> None:
        # varintPacked.h: #if PACK_STORAGE_BITS, with
        # #define PACK_STORAGE_BITS DIRECT_STORAGE_BITS in the includer
        (self.repo / "deps").mkdir()
        (self.repo / "deps" / "packed.h").write_text("#if PACK_BITS > 64\n#endif\n")
        self.write(
            "src/a.c",
            "#define LANE_BITS 12\n#define PACK_BITS LANE_BITS\n"
            '#include "packed.h"\nint f(void) { return LANE_BITS; }\n',
        )
        self.assertEqual(self.plan()["LANE_BITS"], defconst.PREPROCESSOR)

    def test_a_name_nothing_uses_is_reported_not_converted(self) -> None:
        self.write("src/a.c", "#define USE_NEON 1\nint f(void) { return 0; }\n")
        self.assertEqual(self.plan()["USE_NEON"], defconst.UNUSED)

    def test_a_name_stringified_or_pasted_stays(self) -> None:
        self.write(
            "src/a.c",
            "#define WIDTH 16\n#define NAME_OF(x) #x\n#define S NAME_OF(WIDTH)\n"
            "#define DEPTH 4\n#define GLUE(a) a##DEPTH\n",
        )
        plan = self.plan()
        self.assertEqual(plan["DEPTH"], defconst.PREPROCESSOR)


class Rewrite(unittest.TestCase):
    def test_converts_with_the_literals_type_and_keeps_the_comment(self) -> None:
        src = (
            "#define SMALL_MAX 64\n"
            "#define PACE_NS 20000u /* about 50k/s */\n"
            "#define PO9 1'000'000'000ULL\n"
            "#define KEEP 1\n"
        )
        out = defconst.rewrite(src.encode(), {b"SMALL_MAX", b"PACE_NS", b"PO9"})
        self.assertEqual(
            out.decode(),
            "static constexpr int SMALL_MAX = 64;\n"
            "static constexpr unsigned PACE_NS = 20000u; /* about 50k/s */\n"
            "static constexpr uint64_t PO9 = 1'000'000'000ULL;\n"
            "#define KEEP 1\n",
        )


class Converges(unittest.TestCase):
    def test_a_converted_constant_the_rules_now_keep_goes_back(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "src").mkdir()
            (repo / "deps").mkdir()
            (repo / "deps" / "packed.h").write_text("#if PACK_BITS > 64\n#endif\n")
            path = repo / "src" / "a.c"
            path.write_text(
                "static constexpr int PACK_BITS = 12; /* lane width */\n"
                "static constexpr unsigned KEEP = 4u;\n"
                "int f(void) { return KEEP; }\n"
            )
            with contextlib.redirect_stdout(io.StringIO()):
                defconst.run(repo, ("src/a.c",), False)
            self.assertEqual(
                path.read_text(),
                "#define PACK_BITS 12 /* lane width */\n"
                "static constexpr unsigned KEEP = 4u;\n"
                "int f(void) { return KEEP; }\n",
            )

    def test_local_constexpr_constants_of_one_name_stay(self) -> None:
        # a constant per function is fine as constexpr; only a constant
        # the preprocessor reads goes back to #define
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "src").mkdir()
            path = repo / "src" / "a.c"
            src = "static constexpr int rowMax = 64;\nint f(void) { return rowMax; }\n"
            path.write_text(src)
            (repo / "src" / "b.c").write_text(
                "static constexpr int rowMax = 32;\nint g(void) { return rowMax; }\n"
            )
            with contextlib.redirect_stdout(io.StringIO()):
                defconst.run(repo, ("src/a.c",), False)
            self.assertEqual(path.read_text(), src)


class Files(unittest.TestCase):
    def test_report_changes_nothing(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "src").mkdir()
            src = "#define N 3\nint f(void) { return N; }\n"
            (repo / "src" / "a.c").write_text(src)
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                defconst.run(repo, ("src/a.c",), True)
            self.assertEqual((repo / "src" / "a.c").read_text(), src)
            self.assertIn("src/a.c: 1 to constexpr, 0 back to #define", buf.getvalue())


if __name__ == "__main__":
    unittest.main()
