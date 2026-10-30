"""``dktools profile``: the profiler output parsers and the tables.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import unittest
from pathlib import Path

from dktools import profile
from dktools.gitutil import ToolError

SAMPLE = """Analysis of sampling datakit-test (pid 123) every 1 millisecond
Process:         datakit-test [123]

Call graph:
    10 Thread_9001   DispatchQueue_1: com.apple.main-thread  (serial)
      10 start  (in dyld) + 6076  [0x18acb0274]
        10 main  (in datakit-test) + 100  [0x1000]
          6 flexFind  (in datakit-test) + 8  [0x2000]
          + 4 flexFind  (in datakit-test) + 8  [0x2000]
          + ! 4 databoxCompare  (in datakit-test) + 16  [0x3000]
          + 2 databoxCompare  (in datakit-test) + 16  [0x3000]
          4 benchLoop  (in datakit-test) + 4  [0x4000]
    2 Thread_9002
      2 _pthread_start  (in libsystem_pthread.dylib) + 136  [0x18]

Total number in stack (recursive counted multiple, when >=5):
        10       main  (in datakit-test) + 100  [0x1000]

Sort by top of stack, same collapsed (when >= 5):
        databoxCompare  (in datakit-test)        6
        benchLoop  (in datakit-test)        4
        _pthread_start  (in libsystem_pthread.dylib)        2

Binary Images:
       0x1000 - 0x9000 datakit-test
"""

PERF_SCRIPT = """datakit-test 123 4.5: 250000 cpu-clock:
\t    10a0 databoxCompare+0x10 (/x/datakit-test)
\t    20b0 flexFind+0x8 (/x/datakit-test)
\t    30c0 main+0x4 (/x/datakit-test)

datakit-test 123 4.6: 250000 cpu-clock:
\t    20b0 flexFind+0x8 (/x/datakit-test)
\t    30c0 main (/x/datakit-test)
"""


class Parsers(unittest.TestCase):
    def test_sample_report_tree_and_top_of_stack(self) -> None:
        s = profile.parse_sample(SAMPLE)
        # the threads' roots: 10 + 2
        self.assertEqual(s.total, 12)
        self.assertEqual(s.inclusive["main"], 10)
        # flexFind recursing under itself counts once: its outer node's 6
        self.assertEqual(s.inclusive["flexFind"], 6)
        # databoxCompare under the inner and the outer flexFind: 4 + 2
        self.assertEqual(s.inclusive["databoxCompare"], 6)
        self.assertEqual(s.inclusive["benchLoop"], 4)
        self.assertNotIn("Thread_9001", s.inclusive)
        # self weights from the tree (a node less its children), nothing
        # from the "Total number in stack" list or the image table
        self.assertEqual(
            dict(s.self_weight),
            {"databoxCompare": 6, "benchLoop": 4, "_pthread_start": 2},
        )

    def test_an_excluded_frame_leaves_with_its_subtree(self) -> None:
        held = SAMPLE.replace("4 benchLoop", "4 profileHoldAtExit_")
        s = profile.parse_sample(held, exclude=("profileHoldAtExit_",))
        # its 4 come off every node above it: the thread, start, main
        self.assertEqual(s.total, 8)
        self.assertEqual(s.inclusive["main"], 6)
        self.assertEqual(s.inclusive["start"], 6)
        self.assertNotIn("profileHoldAtExit_", s.inclusive)
        self.assertNotIn("profileHoldAtExit_", s.self_weight)
        self.assertEqual(s.self_weight["databoxCompare"], 6)

    def test_reports_merge_into_one_profile(self) -> None:
        a = profile.parse_sample(SAMPLE)
        a.merge(profile.parse_sample(SAMPLE))
        self.assertEqual(a.total, 24)
        self.assertEqual(a.inclusive["main"], 20)
        self.assertEqual(a.self_weight["benchLoop"], 8)

    def test_perf_script_reads_leaf_first_symbols(self) -> None:
        s = profile.parse_perf_script(PERF_SCRIPT)
        self.assertEqual(s.total, 2)
        self.assertEqual(dict(s.self_weight), {"databoxCompare": 1, "flexFind": 1})
        self.assertEqual(s.inclusive["main"], 2)
        self.assertEqual(s.inclusive["databoxCompare"], 1)


class Render(unittest.TestCase):
    def test_percentages_and_top(self) -> None:
        s = profile.Samples()
        s.add(["a", "b"], 3)
        s.add(["b"], 1)
        self.assertEqual(
            profile.render(s, top=1),
            [
                "4 sample weight",
                "self:",
                "   75.00%  a",
                "inclusive:",
                "  100.00%  b",
            ],
        )

    def test_empty(self) -> None:
        self.assertEqual(profile.render(profile.Samples(), top=5), ["0 sample weight"])


class Refusals(unittest.TestCase):
    def test_unknown_revision_refuses_before_building(self) -> None:
        repo = Path(__file__).resolve().parents[2]
        opts = profile.ProfileOptions(
            name="t", modules="idRows", rev="no-such-revision-anywhere"
        )
        with self.assertRaises(ToolError):
            profile.run(repo, opts)


if __name__ == "__main__":
    unittest.main()
