"""``dktools docexamples``: the docs' C examples call datakit as it is.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from dktools import docexamples

_DOC = """# A module

Some prose.

```c
flex *f = flexNew();
flexPushBytes(&f, "x", 1, FLEX_ENDPOINT_TAIL);
```

```c
#include "flex.h"
static void helper(void) {
    flexFree(flexNew());
}
```

```text
not C
```
"""


class DocExamples(unittest.TestCase):
    def setUp(self) -> None:
        self._dir = tempfile.TemporaryDirectory()
        self.repo = Path(self._dir.name)
        (self.repo / "docs").mkdir()
        (self.repo / "docs" / "A.md").write_text(_DOC)
        (self.repo / "src").mkdir()
        (self.repo / "src" / "flex.h").write_text(
            "#pragma once\nflex *flexNew(void);\nvoid flexFree(flex *f);\n"
            "#define FLEX_ENDPOINT_TAIL 1\n"
        )

    def tearDown(self) -> None:
        self._dir.cleanup()

    def test_blocks_are_the_c_fences_at_their_doc_lines(self) -> None:
        found = docexamples.blocks(self.repo)
        self.assertEqual([b.line for b in found], [6, 11])
        self.assertTrue(found[0].code.startswith("flex *f = flexNew();"))

    def test_a_fragment_is_a_function_body_a_file_is_a_file(self) -> None:
        found = docexamples.blocks(self.repo)
        declared = {"flexNew": "flex.h", "flexFree": "flex.h"}
        fragment, first = docexamples.unit_text(found[0], declared)
        lines = fragment.splitlines()
        self.assertIn('#include "flex.h"', lines)
        self.assertEqual(lines[first - 1], "flex *f = flexNew();")
        self.assertEqual(lines[first - 2], "static void dkDocExample_(void) {")
        whole, first = docexamples.unit_text(found[1], declared)
        self.assertNotIn("dkDocExample_", whole)
        self.assertEqual(whole.splitlines()[first - 1], '#include "flex.h"')

    def test_only_errors_that_prove_staleness_are_kept(self) -> None:
        block = docexamples.Block("docs/A.md", 6, "x")
        stderr = (
            "/t/unit.c:12:5: error: use of undeclared identifier 'm'\n"
            "/t/unit.c:12:9: error: too many arguments to function call, "
            "expected 2, have 5\n"
            "/t/unit.c:13:1: error: use of undeclared identifier "
            "'FLEX_CAP_LEVEL_4096'\n"
            "/t/unit.c:13:1: error: use of undeclared identifier 'MY_THING'\n"
            "/t/unit.c:14:1: error: call to undeclared function 'flexPush'\n"
            "/t/unit.c:3:1: error: no member named 'x' in prelude\n"
            "/t/unit.c:15:1: error: incompatible pointer types passing "
            "'int (*)(int, int, off_t)' to parameter of type "
            "'pthread_mutex_t *' [-Wincompatible-pointer-types]\n"
            "/t/unit.c:16:1: error: incompatible pointer types passing "
            "'time_t (time_t *)' (aka 'long (long *)') to parameter of type "
            "'multiTimer *' (aka 'struct multiTimer *')\n"
        )
        got = docexamples.proofs(stderr, block, 12, frozenset({"FLEX"}))
        self.assertEqual(
            [(f.line, f.message.split(",")[0]) for f in got],
            [
                (6, "too many arguments to function call"),
                (7, "use of undeclared identifier 'FLEX_CAP_LEVEL_4096'"),
                (8, "call to undeclared function 'flexPush'"),
            ],
        )

    def test_undeclared_datakit_names_are_stale_and_the_readers_are_not(
        self,
    ) -> None:
        stems = frozenset({"flex"})
        prefixes = frozenset({"FLEX"})
        self.assertTrue(docexamples.stale_name("flexGone", prefixes, stems))
        self.assertTrue(docexamples.stale_name("FLEX_GONE", prefixes, stems))
        self.assertFalse(docexamples.stale_name("flex", prefixes, stems))
        self.assertFalse(docexamples.stale_name("myThing", prefixes, stems))
        self.assertFalse(docexamples.stale_name("MY_THING", prefixes, stems))
        self.assertFalse(docexamples.stale_name("flex_buf", prefixes, stems))
        block = docexamples.Block("d", 1, "flexGone(1);\nint n = flexCount;\n")
        stderr = (
            "/t/unit.c:5:1: error: use of undeclared identifier 'flexGone'\n"
            "/t/unit.c:6:9: error: use of undeclared identifier 'flexCount'\n"
        )
        got = docexamples.proofs(stderr, block, 5, prefixes, stems)
        self.assertEqual([f.line for f in got], [1])

    def test_links_need_their_file_and_heading(self) -> None:
        self.assertEqual(
            docexamples.slug("Cluster mailbox — verified-frame router"),
            "cluster-mailbox--verified-frame-router",
        )
        self.assertEqual(
            docexamples.slug("`clusterReplicator.h`"), "clusterreplicatorh"
        )
        (self.repo / "docs" / "B.md").write_text(
            "# Top\n\n## Part one\n\n## Part one\n\n```c\n# not a heading\n```\n"
        )
        (self.repo / "docs" / "C.md").write_text(
            "[a](B.md#part-one) [b](B.md#part-one-1) [c](B.md#nope) "
            "[d](Gone.md) [e](https://x.y/z) [f](#here)\n\n## Here\n"
        )
        got = docexamples.broken_links(self.repo, [self.repo / "docs" / "C.md"])
        self.assertEqual(
            [f.message for f in got],
            ["link to a missing heading: B.md#nope", "link to a missing file: Gone.md"],
        )

    def test_what_not_to_do_is_not_checked(self) -> None:
        code = (
            "/* WRONG - no such call */\nbad(1);\n"
            "/* RIGHT */\ngood(1);\n// AVOID: this\nworse();\n"
        )
        self.assertEqual(docexamples.wrong_lines(code), {1, 2, 5, 6})
        block = docexamples.Block("docs/A.md", 10, code)
        stderr = (
            "/t/unit.c:21:1: error: call to undeclared function 'bad'\n"
            "/t/unit.c:23:1: error: call to undeclared function 'good'\n"
        )
        got = docexamples.proofs(stderr, block, 20, frozenset())
        self.assertEqual([f.line for f in got], [13])

    def test_headers_map_each_call_to_its_header(self) -> None:
        headers = {"src/flex.h": (self.repo / "src" / "flex.h").read_text()}
        self.assertEqual(
            docexamples.declaring_headers(headers),
            {"flexNew": "flex.h", "flexFree": "flex.h"},
        )
        self.assertIn("FLEX", docexamples.constant_prefixes(headers))
        self.assertEqual(
            docexamples.defining_headers(headers), {"FLEX_ENDPOINT_TAIL": "flex.h"}
        )
        more = {
            "src/m.h": "#pragma once\n#define mPopHead(m) mPop(m, false)\n"
            "typedef struct mEntry { int x; } mEntry;\n"
            "static inline int mSize(const int *m) { return *m; }\n"
        }
        self.assertEqual(
            docexamples.defining_headers(more),
            {"mPopHead": "m.h", "mEntry": "m.h", "mSize": "m.h"},
        )
        fragment, _ = docexamples.unit_text(
            docexamples.Block("d", 1, "x = FLEX_ENDPOINT_TAIL;"),
            {},
            docexamples.defining_headers(headers),
        )
        self.assertIn('#include "flex.h"', fragment)


if __name__ == "__main__":
    unittest.main()
