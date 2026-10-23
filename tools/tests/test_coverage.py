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


if __name__ == "__main__":
    unittest.main()
