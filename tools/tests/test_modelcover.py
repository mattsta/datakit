import subprocess
import tempfile
import unittest
from pathlib import Path

from dktools import modelcover
from dktools.gitutil import ToolError

HEADER = """#pragma once
typedef struct box box;
box *boxNew(void);
void boxFree(box *b);
bool boxPut(box *b, int key);
bool boxSetSeed(box *b, unsigned seed);
void boxRepr(const box *b);
#ifdef DATAKIT_TEST
int boxTest(int argc, char *argv[]);
#endif
"""

SOURCE = """#include "box.h"
bool boxPut(box *b, int key) { return key > 0; }

#ifdef DATAKIT_TEST
/* @model: src/box.h */
static int boxModel_(void) {
    cleaner(boxFree) box *b = boxNew();
    boxPut(b, 1);
    boxModelStep_(b);
    return 0;
}

/* @model: src/box.h */
static void boxModelStep_(box *b) {
    (void)boxPut(b, 2);
}

static void notAModel_(box *b) {
    boxSetSeed(b, 3);
}
#endif
"""


class CoverageTests(unittest.TestCase):
    def test_calls_outside_the_marked_functions_do_not_count(self):
        models = modelcover.models({"src/box.c": SOURCE})
        self.assertEqual(set(models), {"src/box.h"})
        self.assertEqual(
            models["src/box.h"].called >= {"boxNew", "boxPut", "boxFree"}, True
        )
        self.assertNotIn("boxSetSeed", models["src/box.h"].called)

    def test_a_cleaner_scope_counts_as_a_call_of_its_function(self):
        models = modelcover.models({"src/box.c": SOURCE})
        self.assertIn("boxFree", models["src/box.h"].called)
        named = SOURCE.replace("cleaner(boxFree)", "cleanerName(boxFree, Box)")
        self.assertIn(
            "boxFree", modelcover.models({"src/box.c": named})["src/box.h"].called
        )

    def test_missing_calls_are_the_public_ones_no_model_makes(self):
        missing = modelcover.missing(
            {"src/box.h": HEADER}, modelcover.models({"src/box.c": SOURCE})
        )
        self.assertEqual(missing, {"src/box.h": ["boxRepr", "boxSetSeed"]})


class RunTests(unittest.TestCase):
    def _repo(self, allow: str) -> Path:
        root = Path(tempfile.mkdtemp())
        (root / "src").mkdir()
        (root / "tools").mkdir()
        (root / "src/box.h").write_text(HEADER)
        (root / "src/box.c").write_text(SOURCE)
        (root / "src/plain.h").write_text("#pragma once\nint plainGet(void);\n")
        (root / modelcover.ALLOW_FILE).write_text(allow)
        subprocess.run(["git", "init", "-q"], cwd=root, check=True)
        subprocess.run(["git", "add", "-A"], cwd=root, check=True)
        return root

    def test_unallowed_gaps_fail_and_allowed_ones_pass(self):
        self.assertEqual(modelcover.run(self._repo(""), ()), 1)
        repo = self._repo(
            "src/box.h boxRepr prints, changes nothing\n"
            "src/box.h boxSetSeed checked by its own test\n"
        )
        self.assertEqual(modelcover.run(repo, ()), 0)

    def test_a_stale_allow_line_fails(self):
        repo = self._repo(
            "src/box.h boxRepr prints, changes nothing\n"
            "src/box.h boxSetSeed checked by its own test\n"
            "src/box.h boxPut the model makes it now\n"
        )
        self.assertEqual(modelcover.run(repo, ()), 1)

    def test_an_allow_line_needs_a_reason(self):
        with self.assertRaises(ToolError):
            modelcover.parse_allow("src/box.h boxRepr\n")


if __name__ == "__main__":
    unittest.main()
