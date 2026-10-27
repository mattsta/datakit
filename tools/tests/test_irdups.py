"""``dktools irdups``: IR parsing, normalising and grouping.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from dktools import irdups
from dktools.gitutil import ToolError

ADD = """\
define dso_local i32 @{name}(i32 noundef %{a}, i32 noundef %{b}) local_unnamed_addr #0 !dbg !7 {{
entry:
  %{s} = add nsw i32 %{b}, %{a}, !dbg !12
  %{t} = icmp sgt i32 %{s}, 0
  br i1 %{t}, label %{yes}, label %{no}, !prof !9

{yes}:                                              ; preds = %entry
  %{c} = call i32 @{callee}(i32 %{s}) #3
  ret i32 %{c}

{no}:
  ret i32 0
}}
"""


def body(name: str, callee: str = "helper", **names: str) -> str:
    fields = dict(a="0", b="1", s="2", t="3", yes="4", no="5", c="6")
    fields.update(names)
    return ADD.format(name=name, callee=callee, **fields)


def digests(ir: str) -> dict[str, str]:
    return {b.name: b.digest for b in irdups.bodies_of("u.c", ir, 1)}


class Normalise(unittest.TestCase):
    def test_bodies_are_found_with_their_names(self) -> None:
        ir = "@g = global i32 0\n" + body("first") + "\n" + body("second")
        found = irdups.function_bodies(ir)
        self.assertEqual([name for name, _ in found], ["first", "second"])
        self.assertEqual(found[0][1][-1], "}")

    def test_names_metadata_and_attributes_do_not_count(self) -> None:
        plain = body("f")
        renamed = body(
            "g", a="x", b="y", s="sum", t="cmp", yes="if.then", no="if.end", c="r"
        )
        renamed = renamed.replace(", !dbg !12", "").replace(" #3", " #8")
        got = digests(plain + "\n" + renamed)
        self.assertEqual(got["f"], got["g"])
        _, count = irdups.normalise(irdups.function_bodies(plain)[0][1])
        self.assertEqual(count, 6)  # add, icmp, br, call, ret, ret

    def test_callees_types_and_operand_order_count(self) -> None:
        base = digests(body("f"))["f"]
        self.assertNotEqual(base, digests(body("g", callee="other"))["g"])
        self.assertNotEqual(base, digests(body("g").replace("i32 0", "i32 1"))["g"])
        swapped = body("g").replace("%1, %0", "%0, %1")
        self.assertNotEqual(base, digests(swapped)["g"])

    def test_string_constant_numbering_does_not_count(self) -> None:
        a = body("f").replace("@helper(i32 %6)", "@helper(ptr @.str.3)")
        b = body("g").replace("@helper(i32 %6)", "@helper(ptr @.str)")
        got = digests(a + "\n" + b)
        self.assertEqual(got["f"], got["g"])


class Groups(unittest.TestCase):
    def test_one_name_in_many_units_is_not_a_group(self) -> None:
        bodies = irdups.bodies_of("a.c", body("inl"), 1)
        bodies += irdups.bodies_of("b.c", body("inl.12"), 1)
        self.assertEqual(irdups.groups(bodies), [])

    def test_two_names_group_and_short_bodies_drop(self) -> None:
        bodies = irdups.bodies_of("a.c", body("one"), 1)
        bodies += irdups.bodies_of("b.c", body("two"), 1)
        found = irdups.groups(bodies)
        self.assertEqual(
            [[(b.unit, b.name) for b in g] for g in found],
            [[("a.c", "one"), ("b.c", "two")]],
        )
        self.assertEqual(irdups.bodies_of("a.c", body("one"), 7), [])

    def test_type_template_twins_are_told_apart(self) -> None:
        def group(*names: str) -> list[irdups.Body]:
            return [irdups.Body("u.c", n, 1, "d") for n in names]

        self.assertTrue(
            irdups.template_twins(group("fenwickI16Cells_", "fenwickU32Cells_"))
        )
        self.assertTrue(
            irdups.template_twins(group("segFloatLeaves_", "segDoubleLeaves_"))
        )
        self.assertFalse(irdups.template_twins(group("mosWideOwned_", "mdWideOwned_")))
        self.assertFalse(irdups.template_twins(group("g4", "g_quartet")))


class Commands(unittest.TestCase):
    def test_ir_command_swaps_output_optimisation_and_debug(self) -> None:
        cmd = (
            "/opt/clang -DDATAKIT_TEST -O3 -g -fsanitize=address -I/x "
            "-o CMakeFiles/t.dir/src/a.c.o -c /r/src/a.c"
        )
        self.assertEqual(
            irdups.ir_command(cmd),
            [
                "/opt/clang",
                "-DDATAKIT_TEST",
                "-I/x",
                "/r/src/a.c",
                "-O1",
                "-fno-inline",
                "-g0",
                "-S",
                "-emit-llvm",
                "-o",
                "-",
            ],
        )

    def test_database_prefers_the_test_target(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "build/dev").mkdir(parents=True)
            entries = [
                {
                    "directory": "/b",
                    "file": str(repo / "src/a.c"),
                    "command": "cc -O3 -c src/a.c",
                },
                {
                    "directory": "/b",
                    "file": str(repo / "src/a.c"),
                    "command": "cc -DDATAKIT_TEST -c src/a.c",
                },
                {
                    "directory": "/b",
                    "file": str(repo / "src/b.c"),
                    "arguments": ["cc", "-c", "src/b.c"],
                },
                {"directory": "/b", "file": "/elsewhere/c.c", "command": "cc -c c.c"},
            ]
            (repo / "build/dev/compile_commands.json").write_text(json.dumps(entries))
            got = irdups.database_entries(repo, "dev")
            self.assertEqual(sorted(got), ["src/a.c", "src/b.c"])
            self.assertIn("DATAKIT_TEST", got["src/a.c"][1])
            self.assertEqual(got["src/b.c"][1], "cc -c src/b.c")
            with self.assertRaises(ToolError):
                irdups.database_entries(repo, "missing")


if __name__ == "__main__":
    unittest.main()
