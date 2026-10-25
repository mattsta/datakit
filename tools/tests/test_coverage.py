"""``dktools coverage``: header declarations, profile counts, the llvm-cov
summary, the public-API split and the report.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import json
import unittest
from pathlib import Path

from dktools import coverage

HEADER = """
#pragma once
/* A comment with fake(void); inside */
#include "databox.h"
#define MACRO(x) do_not_count(x)
#define LONG_MACRO(x) \\
    also_not(x)

typedef struct widget widget;
typedef bool (*widgetVisitor)(widget *w, void *ctx);

widget *widgetNew(void);
void widgetFree(widget *w);
size_t widgetCount(const widget *w); // trailing comment
bool widgetInsert(widget **w,
                  const databox *key,
                  const databox *val);
flexEntry DK_FN_PURE *widgetFirst(const widget *w);

static inline bool widgetEmpty(const widget *w) {
    return widgetCount(w) == 0;
}

typedef struct widgetIter {
    widget *w;
    void (*step)(struct widgetIter *it);
} widgetIter;

FENWICK_DECLARE_TYPE(I64, int64_t, uint32_t)

DKS_TYPE *DKS_NEW(const char *init);
void *calloc(size_t count, size_t size) __attribute__((deprecated));

#ifdef DATAKIT_TEST
int widgetTest(int argc, char *argv[]);
#endif
"""

#: Where a coverage run exported the tree (llvm-cov reports absolute paths)
EXPORT_ROOT = "/coverage/export/src"

PROFILE = """Counters:
  widgetNew:
    Hash: 0x0000000000000018
    Counters: 1
    Function count: 12
    Block counts: []
  widget.c:helper_:
    Hash: 0x0000000000000018
    Counters: 1
    Function count: 3
    Block counts: []
  widgetFree:
    Hash: 0x0000000000000018
    Counters: 1
    Function count: 0
    Block counts: []
Instrumentation level: Front-end
"""


def summary(root: str) -> str:
    def s(lines, functions):
        return {
            "lines": {"count": lines[1], "covered": lines[0]},
            "functions": {"count": functions[1], "covered": functions[0]},
            "branches": {"count": 4, "covered": 2},
            "regions": {"count": 8, "covered": 4},
        }

    return json.dumps(
        {
            "data": [
                {
                    "files": [
                        {
                            "filename": f"{root}/src/widget.c",
                            "summary": s((5, 10), (1, 2)),
                        },
                        {
                            "filename": f"{root}/src/gadget.c",
                            "summary": s((9, 10), (2, 2)),
                        },
                    ],
                    "totals": s((14, 20), (3, 4)),
                }
            ]
        }
    )


INLINE_HEADER = """
#pragma once
static inline uint64_t treeLeft(uint64_t i) { return i << 1; }
DK_INLINE_ALWAYS bool treeEmpty(const tree *t) {
    return t->count == 0;
}
static inline void treeNeverCompiled(void) {}
#define TREE_TEMPLATE(T) static inline T T##Make(void) { return 0; }
"""

#: treeLeft runs in one of its two copies, treeEmpty in neither
INLINE_PROFILE = """Counters:
  a.c:treeLeft:
    Function count: 0
  b.c:treeLeft:
    Function count: 7
  a.c:treeEmpty:
    Function count: 0
  b.c:treeEmpty:
    Function count: 0
"""


class Coverage(unittest.TestCase):
    def test_inline_functions_are_header_definitions(self) -> None:
        self.assertEqual(
            coverage.inline_functions(INLINE_HEADER),
            ["treeEmpty", "treeLeft", "treeNeverCompiled"],
        )
        # A prototype is not a definition
        self.assertEqual(coverage.inline_functions(HEADER), ["widgetEmpty"])

    def test_unrun_inline_sums_every_copy(self) -> None:
        counts = coverage.parse_profile_counts(INLINE_PROFILE)
        self.assertEqual(
            coverage.unrun_inline({"src/tree.h": INLINE_HEADER}, counts),
            {"src/tree.h": ["treeEmpty"]},
        )

    def test_report_renders_from_a_work_dir(self) -> None:
        import contextlib
        import io
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            (work / "logs").mkdir()
            (work / "src" / "src").mkdir(parents=True)
            (work / "src" / "src" / "tree.h").write_text(INLINE_HEADER)
            (work / "logs" / "export.json").write_text(
                "$ llvm-cov export\n" + summary(str(work / "src"))
            )
            (work / "logs" / "functions.txt").write_text(INLINE_PROFILE)
            with contextlib.redirect_stdout(io.StringIO()):
                coverage.report(work, "a" * 40, "subject", "all passed", True, 5)
            text = (work / "summary.md").read_text()
            self.assertIn("- `src/tree.h`: treeEmpty", text)
            doc = json.loads((work / "summary.json").read_text())
            self.assertEqual(
                doc["publicApi"]["unrunInline"], {"src/tree.h": ["treeEmpty"]}
            )

    def test_a_regressed_run_is_not_recorded_unless_accepted(self) -> None:
        import contextlib
        import io
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp) / "work"
            repo = Path(tmp) / "repo"
            (repo / "tools").mkdir(parents=True)
            (work / "logs").mkdir(parents=True)
            (work / "src" / "src").mkdir(parents=True)
            (work / "src" / "tools").mkdir()
            (work / "src" / "src" / "spin.c").write_text(SPIN)
            (work / "src" / "src" / "tree.h").write_text(INLINE_HEADER)
            (work / "logs" / "export.json").write_text(
                "$ llvm-cov export\n" + summary(str(work / "src"))
            )
            (work / "logs" / "functions.txt").write_text(INLINE_PROFILE)
            (work / "logs" / "coverage.lcov").write_text(spin_lcov(work / "src", False))
            # The baseline had every point of spin.c but line 3 covered
            base = {
                "commit": "b" * 40,
                "files": {"src/spin.c": {"lines": 1, "branches": 1}},
            }
            (work / "src" / coverage.BASELINE).write_text(json.dumps(base))
            target = repo / coverage.BASELINE
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                held = coverage.report(
                    work, "c" * 40, "s", "all passed", True, 5, repo, True
                )
            self.assertFalse(held)
            self.assertFalse(target.exists())
            self.assertIn("NOT written", out.getvalue())
            with contextlib.redirect_stdout(io.StringIO()):
                coverage.report(
                    work,
                    "c" * 40,
                    "s",
                    "all passed",
                    True,
                    5,
                    repo,
                    True,
                    accept_regressions=True,
                )
            doc = json.loads(target.read_text())
        self.assertEqual(doc["files"]["src/spin.c"], {"lines": 2, "branches": 2})

    def test_declared_functions(self) -> None:
        names = coverage.declared_functions(HEADER)
        self.assertEqual(
            names,
            [
                "widgetNew",
                "widgetFree",
                "widgetCount",
                "widgetInsert",
                "widgetFirst",
                "widgetTest",
            ],
        )

    def test_profile_counts_keep_static_names_apart(self) -> None:
        self.assertEqual(
            coverage.parse_profile_counts(PROFILE),
            {"widgetNew": 12, "widget.c:helper_": 3, "widgetFree": 0},
        )

    def test_export_summary(self) -> None:
        root = Path(EXPORT_ROOT)
        files, total = coverage.parse_export_summary(summary(str(root)), root)
        self.assertEqual([f.path for f in files], ["src/widget.c", "src/gadget.c"])
        self.assertEqual(files[0].lines, (5, 10))
        self.assertEqual(total.functions, (3, 4))
        self.assertAlmostEqual(files[0].line_percent, 50.0)

    def test_public_api_split(self) -> None:
        counts = coverage.parse_profile_counts(PROFILE)
        api = coverage.public_api({"src/widget.h": HEADER}, counts)
        self.assertEqual(api.called, {"src/widget.h": ["widgetNew"]})
        self.assertEqual(api.never, {"src/widget.h": ["widgetFree"]})
        # Declared but absent from the profile; the Test entry point is out
        self.assertEqual(
            api.not_built,
            {"src/widget.h": ["widgetCount", "widgetFirst", "widgetInsert"]},
        )

    def test_render_lists_gaps_least_covered_first(self) -> None:
        root = Path(EXPORT_ROOT)
        files, total = coverage.parse_export_summary(summary(str(root)), root)
        api = coverage.public_api(
            {"src/widget.h": HEADER}, coverage.parse_profile_counts(PROFILE)
        )
        text = coverage.render("a" * 40, "subject", "all passed", files, total, api, 5)
        self.assertIn("| 70.0% (14/20) |", text)
        self.assertLess(text.index("src/widget.c"), text.index("src/gadget.c"))
        self.assertIn(
            "5 declared: 1 called by the tests, 1 never called, 3 not built", text
        )
        self.assertIn("- `src/widget.h`: widgetFree", text)
        doc = coverage.to_json("a" * 40, True, files, total, api)
        self.assertEqual(
            doc["publicApi"]["neverCalled"], {"src/widget.h": ["widgetFree"]}
        )

    def test_private_headers_are_not_api(self) -> None:
        self.assertTrue(coverage.PRIVATE_HEADER.search("src/multimapFullInternal.h"))
        self.assertTrue(
            coverage.PRIVATE_HEADER.search("src/fenwick/fenwick2dCoreImpl.h")
        )
        self.assertTrue(
            coverage.PRIVATE_HEADER.search("src/memshard/include/mimalloc/types.h")
        )
        self.assertTrue(coverage.PRIVATE_HEADER.search("src/memshard/src/bitmap.h"))
        self.assertFalse(
            coverage.PRIVATE_HEADER.search("src/memshard/include/mimalloc.h")
        )
        self.assertFalse(coverage.PRIVATE_HEADER.search("src/multimap.h"))


SHOW = """\
    1|       |/* widget.c */
    2|      5|int widgetGet(int x) {
    3|      5|    if (x < 0) {
    4|      0|        return -1;
    5|       |        /* unreachable note */
    6|      0|    }
    7|  1.20k|    return x;
    8|      5|}
    9|       |
   10|      0|void widgetFree(void) {
   11|      0|}
"""

REPORT = """\
File '/x/src/widget.c':
Name                                     Regions    Miss   Cover     Lines    Miss   Cover  Branches    Miss   Cover
--------------------------------------------------------------------------------------------------------------------
widgetGet                                      4       1  75.00%         6       2  66.67%         2       1  50.00%
widgetFree                                     1       1   0.00%         2       2   0.00%         0       0   0.00%
widgetOk                                       1       0 100.00%         3       0 100.00%         0       0   0.00%
--------------------------------------------------------------------------------------------------------------------
TOTAL                                          6       2  66.67%        11       4  63.64%         2       1  50.00%
"""


LIB_SOURCE = """\
int widgetGet(int x) {          /* 1 */
    return x;                   /* 2 */
}                               /* 3 */
#ifdef DATAKIT_TEST             /* 4 */
#if WIDE                        /* 5 */
int wide;                       /* 6 */
#endif                          /* 7 */
int widgetTest(void) {          /* 8 */
    return widgetGet(1) ? 0 : 1;/* 9 */
}                               /* 10 */
#else                           /* 11 */
int notTest;                    /* 12 */
#endif                          /* 13 */
#ifndef DATAKIT_TEST            /* 14 */
int alsoNotTest;                /* 15 */
#endif                          /* 16 */
#if defined(DATAKIT_TEST) && 1  /* 17 */
int testToo;                    /* 18 */
#endif                          /* 19 */
"""

LCOV = """\
$ llvm-cov export -format=lcov ...
SF:{root}/src/widget.c
FN:1,widgetGet
FN:8,widgetTest
FNDA:4,widgetGet
FNDA:1,widgetTest
DA:1,4
DA:2,4
DA:3,0
DA:8,1
DA:9,0
BRDA:2,0,0,4
BRDA:2,0,1,-
BRDA:9,0,0,0
end_of_record
"""


class LibraryOnly(unittest.TestCase):
    def test_test_regions(self) -> None:
        lines = coverage.test_lines(LIB_SOURCE)
        self.assertEqual(lines, {4, 5, 6, 7, 8, 9, 10, 17, 18})

    def test_lcov_and_library_totals(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "src").mkdir()
            (root / "src" / "widget.c").write_text(LIB_SOURCE)
            lcov = coverage.parse_lcov(LCOV.format(root=root.resolve()), root)
            rec = lcov["src/widget.c"]
            self.assertEqual(rec.lines[9], 0)
            self.assertEqual(rec.functions["widgetTest"], [8, 1])
            files, total = coverage.library_coverage(lcov, root)
        # Lines 8 and 9 and widgetTest are test code: left out
        self.assertEqual(files[0].lines, (2, 3))
        self.assertEqual(files[0].functions, (1, 1))
        self.assertEqual(files[0].branches, (1, 2))
        self.assertEqual(total.lines, (2, 3))


TEMPLATE_HEADER = """\
#if defined(FULL)
#define WTYPE wid
#else
#define WTYPE widc
#endif
#define W_NEW DK_NAME(WTYPE, new)
#define W_LEN DK_NAME(WTYPE, len)
#define W_TEST DK_NAME(WTYPE, Test)
WTYPE *W_NEW(const char *init);
size_t W_LEN(const WTYPE *s);
int W_TEST(int argc, char *argv[]);
"""


class Templates(unittest.TestCase):
    def test_template_names_expand_per_type(self) -> None:
        names = coverage.template_functions(TEMPLATE_HEADER)
        self.assertEqual(
            sorted(names),
            ["widTest", "widcTest", "widclen", "widcnew", "widlen", "widnew"],
        )
        self.assertEqual(coverage.declared_functions(TEMPLATE_HEADER), [])
        api = coverage.public_api(
            {"src/w.h": TEMPLATE_HEADER}, {"widnew": 3, "widcnew": 0}
        )
        self.assertEqual(api.called, {"src/w.h": ["widnew"]})
        self.assertEqual(api.never, {"src/w.h": ["widcnew"]})
        self.assertEqual(api.not_built, {"src/w.h": ["widclen", "widlen"]})


class Ratchet(unittest.TestCase):
    def row(self, path: str, lines: tuple, branches: tuple) -> coverage.FileCoverage:
        return coverage.FileCoverage(path, lines, (0, 0), branches, (0, 0))

    def test_regressions_improvements_and_new_files(self) -> None:
        before = [
            self.row("src/a.c", (90, 100), (40, 50)),
            self.row("src/b.c", (10, 20), (5, 10)),
            self.row("src/c.c", (5, 10), (2, 4)),
        ]
        doc = coverage.baseline_doc("a" * 40, before)
        self.assertEqual(doc["files"]["src/a.c"], {"lines": 10, "branches": 10})
        after = [
            self.row("src/a.c", (88, 100), (40, 50)),  # 2 more lines missed
            self.row("src/b.c", (15, 20), (5, 10)),  # better
            self.row("src/c.c", (5, 10), (2, 4)),  # unchanged
            self.row("src/d.c", (1, 3), (0, 0)),  # new
            self.row("src/e.h", (0, 0), (0, 0)),  # no code
        ]
        r = coverage.ratchet(doc, after)
        self.assertFalse(r.ok)
        self.assertEqual(r.regressions, ["src/a.c: uncovered lines 10 -> 12"])
        self.assertEqual(len(r.improved), 1)
        self.assertTrue(r.improved[0].startswith("src/b.c"))
        self.assertEqual(r.new, ["src/d.c: 2 uncovered lines, 0 branches"])
        text = coverage.render_ratchet(r)
        self.assertIn("REGRESSED", text)
        self.assertIn("no baseline", coverage.render_ratchet(None))

    def test_newly_uncovered_follows_source_across_edits(self) -> None:
        old_text = "int f(int x) {\n  if (x) {\n    return 1;\n  }\n  return 0;\n}\n"
        # Two lines added above: every line moves down by two
        new_text = "// a\n// b\n" + old_text
        old = coverage.LcovFile(
            lines={1: 3, 2: 3, 3: 3, 5: 0},
            branches={(2, 0, 0): True, (2, 0, 1): True},
        )
        new = coverage.LcovFile(
            lines={3: 3, 4: 3, 5: 0, 7: 0},
            branches={(4, 0, 0): False, (4, 0, 1): True},
        )
        lost = coverage.newly_uncovered(old, old_text, new, new_text)
        # line 7 ("return 0;") was uncovered before too: not listed
        self.assertEqual(
            lost,
            [(4, "branch 0.0: if (x) {"), (5, "line: return 1;")],
        )

    def test_report_lists_what_a_regression_lost(self) -> None:
        import tempfile

        text = "int g(int y) {\n  if (y) {\n    return 2;\n  }\n  return 3;\n}\n"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for name in ("base", "head"):
                (root / name / "src" / "src").mkdir(parents=True)
                (root / name / "logs").mkdir()
                (root / name / "src" / "src" / "g.c").write_text(text)
            (root / "base" / "logs" / "coverage.lcov").write_text(
                f"SF:{(root / 'base' / 'src').resolve()}/src/g.c\n"
                "DA:1,1\nDA:3,1\nDA:5,0\nBRDA:2,0,0,1\nBRDA:2,0,1,1\n"
                "end_of_record\n"
            )
            head_src = root / "head" / "src"
            lcov = coverage.parse_lcov(
                f"SF:{head_src.resolve()}/src/g.c\n"
                "DA:1,1\nDA:3,1\nDA:5,0\nBRDA:2,0,0,1\nBRDA:2,0,1,0\n"
                "end_of_record\n",
                head_src,
            )
            files, _ = coverage.library_coverage(lcov, head_src)
            doc = coverage.baseline_doc(
                "base" + "0" * 36, [self.row("src/g.c", (2, 3), (2, 2))]
            )
            r = coverage.ratchet(doc, files)
            self.assertEqual(r.regressed, ["src/g.c"])
            coverage.attach_lost(r, root / "base", lcov, head_src)
            # No cached run at that commit: nothing attached, no error
            other = coverage.ratchet(doc, files)
            coverage.attach_lost(other, root / "missing", lcov, head_src)
        self.assertEqual(r.lost, {"src/g.c": [(2, "branch 0.1: if (y) {")]})
        self.assertIn("src/g.c:2 branch 0.1: if (y) {", coverage.render_ratchet(r))
        self.assertEqual(other.lost, {})
        self.assertIn("re-run coverage", coverage.render_ratchet(other))

    def test_a_file_that_grew_with_tests_holds(self) -> None:
        doc = coverage.baseline_doc("b" * 40, [self.row("src/a.c", (8, 10), (3, 4))])
        r = coverage.ratchet(doc, [self.row("src/a.c", (58, 60), (13, 14))])
        self.assertTrue(r.ok)


class Detail(unittest.TestCase):
    def test_uncovered_ranges_span_lines_without_code(self) -> None:
        ranges = coverage.parse_show_uncovered(SHOW)
        self.assertEqual(
            [(r.first, r.last, r.text) for r in ranges],
            [(4, 6, "return -1;"), (10, 11, "void widgetFree(void) {")],
        )

    def test_functions_by_missed_lines(self) -> None:
        funcs = coverage.parse_function_report(REPORT)
        self.assertEqual(
            [(f.name, f.lines, f.missed) for f in funcs],
            [("widgetFree", 2, 2), ("widgetGet", 6, 2)],
        )

    def test_render(self) -> None:
        text = coverage.render_detail(
            "src/widget.c",
            coverage.parse_function_report(REPORT),
            coverage.parse_show_uncovered(SHOW),
        )
        self.assertIn("- widgetFree: 2 of 2 lines missed", text)
        self.assertIn("- 4-6: `return -1;`", text)
        self.assertIn("every line ran", coverage.render_detail("src/a.c", [], []))


# Two identical "return 0;" lines, told apart by the line before each
SPIN = """\
int spin(int x) {
  if (x) {
    return 0;
  }
  if (x > 1) {
    return 0;
  }
  return 1;
}
"""


def spin_lcov(root: Path, second_taken: bool) -> str:
    """A run of SPIN where line 6 (and its branch) ran or not."""
    six = 1 if second_taken else 0
    return (
        f"SF:{root.resolve()}/src/spin.c\n"
        f"DA:1,2\nDA:2,2\nDA:3,0\nDA:5,2\nDA:6,{six}\nDA:8,2\n"
        f"BRDA:2,0,0,0\nBRDA:2,0,1,2\nBRDA:5,0,0,{six}\nBRDA:5,0,1,2\n"
        "end_of_record\n"
    )


class Volatile(unittest.TestCase):
    def test_volatile_points_leave_the_counts_and_nothing_else(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "src").mkdir()
            (root / "src" / "spin.c").write_text(SPIN)
            lcov = coverage.parse_lcov(spin_lcov(root, False), root)
            plain, _ = coverage.library_coverage(lcov, root)
            flaky = {
                "src/spin.c": {
                    ("line", "return 0;", "if (x > 1) {"),
                    ("branch 0.0", "if (x > 1) {", "}"),
                }
            }
            files, total = coverage.library_coverage(lcov, root, flaky)
        # Line 3 is the same text as line 6 but follows another line: kept
        self.assertEqual(plain[0].lines, (4, 6))
        self.assertEqual(files[0].lines, (4, 5))
        self.assertEqual(plain[0].branches, (2, 4))
        self.assertEqual(files[0].branches, (2, 3))
        self.assertEqual(total.lines, (4, 5))
        doc = coverage.baseline_doc("c" * 40, files, flaky)
        self.assertEqual(coverage.volatile_from(doc), flaky)
        self.assertEqual(doc["files"]["src/spin.c"], {"lines": 1, "branches": 1})

    def test_flipped_points_are_the_runs_differences(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "src").mkdir()
            (root / "src" / "spin.c").write_text(SPIN)
            ran = coverage.parse_lcov(spin_lcov(root, True), root)["src/spin.c"]
            missed = coverage.parse_lcov(spin_lcov(root, False), root)["src/spin.c"]
        self.assertEqual(
            coverage.flipped_points(ran, SPIN, missed, SPIN),
            {
                ("line", "return 0;", "if (x > 1) {"),
                ("branch 0.0", "if (x > 1) {", "}"),
            },
        )
        self.assertEqual(coverage.flipped_points(ran, SPIN, ran, SPIN), set())
        lost = coverage.newly_uncovered(
            ran, SPIN, missed, SPIN, {("line", "return 0;", "if (x > 1) {")}
        )
        self.assertEqual(lost, [(5, "branch 0.0: if (x > 1) {")])

    def test_a_rerun_learns_from_the_run_it_replaced(self) -> None:
        from repofixture import IsolatedGitEnv, TempRepo

        repo = TempRepo()
        try:
            with IsolatedGitEnv():
                repo.write("src/spin.c", SPIN)
                commit = repo.commit("one", "src/spin.c")
                work = repo.path.parent / "coverage" / commit[:10]
                src = work / "src"
                # An unfinished run is not kept
                (work / "logs").mkdir(parents=True)
                coverage.keep_previous_run(work)
                self.assertTrue(work.exists())
                # A finished one moves aside, replacing an older one
                (src / "src").mkdir(parents=True)
                (src / "src" / "spin.c").write_text(SPIN)
                (work / "logs" / "coverage.lcov").write_text(spin_lcov(src, True))
                prev = coverage.previous_run(work)
                (prev / "stale").mkdir(parents=True)
                coverage.keep_previous_run(work)
                self.assertFalse(work.exists())
                self.assertTrue((prev / "logs" / "coverage.lcov").exists())
                self.assertFalse((prev / "stale").exists())
                # The rerun at the same commit, same paths
                (src / "src").mkdir(parents=True)
                (src / "src" / "spin.c").write_text(SPIN)
                lcov = coverage.parse_lcov(spin_lcov(src, False), src)
                learned = coverage.learn_volatile(
                    repo.path, commit, commit, prev, lcov, src, base_root=src
                )
            self.assertEqual(
                learned,
                {
                    "src/spin.c": {
                        ("line", "return 0;", "if (x > 1) {"),
                        ("branch 0.0", "if (x > 1) {", "}"),
                    }
                },
            )
        finally:
            repo.cleanup()

    def test_learning_needs_the_same_sources_and_a_cached_run(self) -> None:
        from dktools.procs import ToolError
        from repofixture import IsolatedGitEnv, TempRepo

        repo = TempRepo()
        try:
            with IsolatedGitEnv():
                repo.write("src/spin.c", SPIN)
                base = repo.commit("base", "src/spin.c")
                repo.write("tools/coverage-baseline.json", "{}\n")
                same = repo.commit("baseline only", "tools/coverage-baseline.json")
                scratch = repo.path.parent / "scratch"
                base_work = scratch / base[:10]
                (base_work / "logs").mkdir(parents=True)
                (base_work / "src" / "src").mkdir(parents=True)
                (base_work / "src" / "src" / "spin.c").write_text(SPIN)
                (base_work / "logs" / "coverage.lcov").write_text(
                    spin_lcov(base_work / "src", True)
                )
                head_src = scratch / same[:10] / "src"
                (head_src / "src").mkdir(parents=True)
                (head_src / "src" / "spin.c").write_text(SPIN)
                lcov = coverage.parse_lcov(spin_lcov(head_src, False), head_src)
                learned = coverage.learn_volatile(
                    repo.path, base, same, base_work, lcov, head_src
                )
                self.assertEqual(
                    learned,
                    {
                        "src/spin.c": {
                            ("line", "return 0;", "if (x > 1) {"),
                            ("branch 0.0", "if (x > 1) {", "}"),
                        }
                    },
                )
                # No cached run of the baseline commit
                with self.assertRaises(ToolError):
                    coverage.learn_volatile(
                        repo.path, base, same, scratch / "none", lcov, head_src
                    )
                # Changed sources: differences are not noise
                repo.write("src/spin.c", SPIN + "\n")
                edited = repo.commit("edit", "src/spin.c")
                with self.assertRaises(ToolError):
                    coverage.learn_volatile(
                        repo.path, base, edited, base_work, lcov, head_src
                    )
        finally:
            repo.cleanup()


if __name__ == "__main__":
    unittest.main()
