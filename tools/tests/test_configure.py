"""``dktools configure``: the cmake command it runs.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import unittest

from dktools import configure, toolchain
from dktools.gitutil import ToolError


class Command(unittest.TestCase):
    def setUp(self) -> None:
        self.clang = toolchain.resolve_compiler("clang22", env={})
        self.gcc = toolchain.resolve_compiler("gcc15", env={})

    def test_a_preset_configures_in_place_with_the_compiler(self) -> None:
        cmd = configure.configure_command(self.clang, "dev")
        self.assertEqual(cmd[:3], ["cmake", "--preset", "dev"])
        self.assertIn(f"-DCMAKE_C_COMPILER={self.clang.cc}", cmd)
        self.assertNotIn("-S", cmd)

    def test_a_preset_the_compiler_does_not_build_is_refused(self) -> None:
        with self.assertRaises(ToolError):
            configure.configure_command(self.gcc, "dev")
        with self.assertRaises(ToolError):
            configure.configure_command(self.gcc, "release")


if __name__ == "__main__":
    unittest.main()
