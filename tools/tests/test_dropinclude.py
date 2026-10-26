"""``dktools dropinclude``: which include lines go, and the file walk.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import contextlib
import io
import tempfile
import unittest
from pathlib import Path

from dktools import dropinclude
from dktools.gitutil import ToolError


class Drops(unittest.TestCase):
    def test_the_header_lines_go_and_others_stay(self) -> None:
        src = (
            b"#include <stdbool.h>\n"
            b"#include <stdint.h>\n"
            b"  #  include <stdbool.h> /* bool */\n"
            b"#include <stdbool.h> // bool\n"
            b'#include "stdbool.h"\n'
            b"/* #include <stdbool.h> */\n"
            b"#include <stdbool.hh>\n"
        )
        out, count = dropinclude.drop(src, "stdbool.h")
        self.assertEqual(count, 3)
        self.assertEqual(
            out,
            b"#include <stdint.h>\n"
            b'#include "stdbool.h"\n'
            b"/* #include <stdbool.h> */\n"
            b"#include <stdbool.hh>\n",
        )

    def test_a_subdirectory_header(self) -> None:
        out, count = dropinclude.drop(b"#include <sys/types.h>\nx\n", "sys/types.h")
        self.assertEqual((out, count), (b"x\n", 1))


class Run(unittest.TestCase):
    def test_given_paths_change_and_dry_run_does_not(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "a.h").write_text("#include <stdbool.h>\nbool b;\n")
            with contextlib.redirect_stdout(io.StringIO()) as out:
                dropinclude.run(repo, "stdbool.h", ("a.h",), True)
            self.assertIn("would remove 1", out.getvalue())
            self.assertIn("stdbool", (repo / "a.h").read_text())
            with contextlib.redirect_stdout(io.StringIO()):
                dropinclude.run(repo, "stdbool.h", ("a.h",), False)
            self.assertEqual((repo / "a.h").read_text(), "bool b;\n")

    def test_bad_names_and_paths_are_refused(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for bad in ("stdbool", "../x.h", "a b.h"):
                with self.assertRaises(ToolError):
                    dropinclude.run(Path(tmp), bad, ("a.h",), True)
            with self.assertRaises(ToolError):
                dropinclude.run(Path(tmp), "stdbool.h", ("missing.h",), True)


if __name__ == "__main__":
    unittest.main()
