import unittest
from pathlib import Path

from dktools import headers
from dktools.gitutil import ToolError


class HeaderCommand(unittest.TestCase):
    def test_reference_command_becomes_a_syntax_check_of_the_unit(self) -> None:
        command = (
            "/opt/clang -DDATAKIT_TEST -I/src -std=c23 -Wall -Werror "
            "-Werror=vla -O2 -MD -MT x.o -MF x.d -o x.o -c /src/datakit.c"
        )
        argv = headers.header_command(command, Path("work/unit.c"))
        self.assertEqual(argv[0], "/opt/clang")
        for kept in ("-DDATAKIT_TEST", "-I/src", "-std=c23", "-Wall", "-O2"):
            self.assertIn(kept, argv)
        for dropped in ("-Werror", "-Werror=vla", "-MD", "-c", "x.o", "x.d"):
            self.assertNotIn(dropped, argv)
        self.assertNotIn("/src/datakit.c", argv)
        self.assertEqual(argv[-4:], ["-fsyntax-only", "-x", "c", "work/unit.c"])


class AllowFile(unittest.TestCase):
    def test_lines_need_a_reason(self) -> None:
        allowed = headers.parse_allow(
            "# comment\n\nsrc/aImpl.h macro template: its includer defines T\n"
        )
        self.assertEqual(
            allowed, {"src/aImpl.h": "macro template: its includer defines T"}
        )
        with self.assertRaises(ToolError):
            headers.parse_allow("src/bImpl.h\n")


class FirstError(unittest.TestCase):
    def test_the_first_error_line_or_the_last_line(self) -> None:
        out = (
            "In file included from unit.c:1:\n"
            "src/x.h:3:5: error: unknown type name 'flexProceed'\n"
            "src/x.h:9:1: error: second\n"
            "2 errors generated.\n"
        )
        self.assertEqual(
            headers.first_error(out),
            "src/x.h:3:5: error: unknown type name 'flexProceed'",
        )
        self.assertEqual(headers.first_error("ld: oops\n"), "ld: oops")
        self.assertEqual(headers.first_error(""), "compile failed")


if __name__ == "__main__":
    unittest.main()
