"""``dktools tune``: sweeps out of perfRegression rows, costs,
recommendations, cache sizes.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import math
import tempfile
import unittest
from pathlib import Path

from dktools import tune
from dktools.gitutil import ToolError


def medians(rows: dict[tuple[str, str, int], float]) -> dict:
    return dict(rows)


class Sweeps(unittest.TestCase):
    def test_limit_rows_group_by_family_op_and_limit(self) -> None:
        found = tune.sweeps(
            medians(
                {
                    ("multimapLimits", "insert256", 2000): 700.0,
                    ("multimapLimits", "exists256", 0): 500.0,
                    ("multimapLimits", "insert1024", 1900): 690.0,
                    ("multimapLimits", "exists1024", 0): 480.0,
                    ("multilistLimits", "popHead2048", 0): 60.0,
                    ("multimap", "insert", 0): 1.0,  # not a sweep module
                    ("multimapLimits", "oddName", 0): 1.0,  # no limit suffix
                }
            )
        )
        self.assertEqual(sorted(found), ["multilist", "multimap"])
        mm = found["multimap"]
        self.assertEqual(mm.ns[256], {"insert": 700.0, "exists": 500.0})
        self.assertEqual(mm.bytes, {256: 2000, 1024: 1900})
        self.assertEqual(mm.ops(), ["insert", "exists"])

    def test_cost_is_the_geometric_mean_of_ratios_to_each_ops_best(self) -> None:
        sweep = tune.Sweep(
            ns={
                512: {"a": 100.0, "b": 200.0},  # best at a, 2x at b
                1024: {"a": 200.0, "b": 100.0},  # 2x at a, best at b
                2048: {"a": 100.0, "b": 100.0},  # best at both
            }
        )
        costs = sweep.costs()
        self.assertAlmostEqual(costs[512], math.sqrt(2.0))
        self.assertAlmostEqual(costs[1024], math.sqrt(2.0))
        self.assertAlmostEqual(costs[2048], 1.0)
        self.assertEqual(sweep.recommend(), (2048, []))

    def test_near_ties_are_listed(self) -> None:
        sweep = tune.Sweep(
            ns={
                512: {"a": 101.0},
                1024: {"a": 100.0},
                4096: {"a": 150.0},
            }
        )
        self.assertEqual(sweep.recommend(), (1024, [512]))

    def test_among_speed_ties_the_densest_wins(self) -> None:
        sweep = tune.Sweep(
            ns={256: {"a": 100.0}, 512: {"a": 101.0}, 1024: {"a": 101.5}},
            bytes={256: 2_560_000, 512: 2_430_000, 1024: 2_340_000},
        )
        self.assertEqual(sweep.recommend(), (1024, [256, 512]))

    def test_render_marks_the_recommendation(self) -> None:
        found = {
            "multimap": tune.Sweep(
                ns={256: {"insert": 2.0}, 512: {"insert": 1.0}}, bytes={512: 99}
            )
        }
        text = "\n".join(tune.render(found, [("L1d", 128 << 10)]))
        self.assertIn("caches    L1d 128 KB", text)
        self.assertIn("<- recommended", text)
        self.assertIn("recommended limit 512", text)
        apply = "dkTierSetDefaultLimit(DK_TIER_MULTIMAP, FLEX_CAP_LEVEL_512);"
        self.assertIn(f"apply: {apply}", text)
        rec = tune.recommendations(found)["multimap"]
        self.assertEqual((rec["limit"], rec["apply"]), (512, apply))

    def test_apply_names_each_familys_setter(self) -> None:
        self.assertEqual(
            tune.apply_text("multiOrderedSet", 2048),
            "dkTierSetDefaultLimit(DK_TIER_MULTIORDEREDSET, FLEX_CAP_LEVEL_2048);",
        )
        self.assertEqual(
            tune.apply_text("multilist", 1024),
            "pass FLEX_CAP_LEVEL_1024 to multilistNew",
        )

    def test_every_setter_names_a_c_family(self) -> None:
        header = Path(__file__).resolve().parents[2] / "src" / "dkTierLimit.h"
        text = header.read_text()
        for family, setter in tune.DEFAULT_SETTERS.items():
            with self.subTest(family=family):
                self.assertIn(family, tune.FAMILIES)
                self.assertIn(setter, text)

    def test_empty_sweep_refuses(self) -> None:
        with self.assertRaises(ToolError):
            tune.Sweep().recommend()


class Caches(unittest.TestCase):
    def test_linux_sysfs(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            for i, (level, kind, size) in enumerate(
                [
                    ("1", "Data", "48K"),
                    ("1", "Instruction", "32K"),
                    ("2", "Unified", "2048K"),
                ]
            ):
                index = root / f"index{i}"
                index.mkdir()
                (index / "level").write_text(level + "\n")
                (index / "type").write_text(kind + "\n")
                (index / "size").write_text(size + "\n")
            self.assertEqual(
                tune.cache_sizes(sys_root=root), [("L1d", 48 << 10), ("L2", 2 << 20)]
            )

    def test_sysctl(self) -> None:
        values = {"hw.perflevel0.l1dcachesize": "131072", "hw.l2cachesize": "4194304"}
        found = tune.cache_sizes(sysctl=values.get, sys_root=Path("/nonexistent-sysfs"))
        self.assertEqual(found, [("L1d (performance cores)", 131072), ("L2", 4194304)])


class Families(unittest.TestCase):
    def test_parse(self) -> None:
        self.assertEqual(tune.parse_families(None), list(tune.FAMILIES))
        self.assertEqual(tune.parse_families("multilist"), ["multilist"])
        for bad in ("nope", ",", "multimap,nope"):
            with self.subTest(bad=bad), self.assertRaises(ToolError):
                tune.parse_families(bad)


if __name__ == "__main__":
    unittest.main()
