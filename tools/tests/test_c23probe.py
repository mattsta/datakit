"""c23probe: call counting, verdicts and the result table.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import unittest

from dktools import c23probe

AARCH64_FOLDED = """\
_g:
\tstp\tx29, x30, [sp, #-16]!
\tbl\t_f
\tlsl\tw0, w0, #1
\tldp\tx29, x30, [sp], #16
\tret
"""

AARCH64_TWICE = """\
_g:
\tbl\t_f
\tmov\tw19, w0
\tbl\t_f
\tadd\tw0, w0, w19
\tret
"""

X86_TWICE = """\
g:
\tcall\tf
\tmovl\t%eax, %ebx
\tcall\tf@PLT
\taddl\t%ebx, %eax
\tret
"""


class CountCalls(unittest.TestCase):
    def test_aarch64(self) -> None:
        self.assertEqual(c23probe.count_calls(AARCH64_FOLDED), 1)
        self.assertEqual(c23probe.count_calls(AARCH64_TWICE), 2)

    def test_x86(self) -> None:
        self.assertEqual(c23probe.count_calls(X86_TWICE), 2)

    def test_tail_call_counts(self) -> None:
        self.assertEqual(c23probe.count_calls("\tb\t_f\n"), 1)
        self.assertEqual(c23probe.count_calls("\tjmp\tf\n"), 1)

    def test_other_symbols_do_not_count(self) -> None:
        asm = "\tbl\t_foo\n\tcall\tfprintf\n\tbl\t_g\n\t.globl\t_f\n_f:\n"
        self.assertEqual(c23probe.count_calls(asm), 0)


class Verdicts(unittest.TestCase):
    def test_calls(self) -> None:
        self.assertEqual(c23probe.calls_verdict(0, AARCH64_FOLDED, ""), "folds")
        self.assertEqual(c23probe.calls_verdict(0, AARCH64_TWICE, ""), "no fold")
        self.assertEqual(
            c23probe.calls_verdict(
                0, AARCH64_TWICE, "x.c:1: warning: unknown attribute\n"
            ),
            "no fold (warns)",
        )
        self.assertEqual(
            c23probe.calls_verdict(1, "", "\nx.c:1:1: error: expected ';'\n"),
            "no: does not compile",
        )
        self.assertEqual(c23probe.calls_verdict(0, "", ""), "no: 0 calls to f")

    def test_compile(self) -> None:
        self.assertEqual(c23probe.compile_verdict(0, ""), "yes")
        self.assertEqual(c23probe.compile_verdict(0, "w\n"), "yes (warns)")
        self.assertEqual(
            c23probe.compile_verdict(1, "e: bad\nmore\n"), "no: does not compile"
        )

    def test_diagnostic_prefers_the_error_line_and_cuts_paths(self) -> None:
        gcc = (
            "/a/b/c23probe/gcc15/memalignment.c: In function 'main':\n"
            "/a/b/c23probe/gcc15/memalignment.c:5:18: error: implicit "
            "declaration of function 'memalignment'\n"
        )
        self.assertEqual(
            c23probe.diagnostic(gcc),
            "memalignment.c:5:18: error: implicit declaration of function "
            "'memalignment'",
        )
        self.assertEqual(c23probe.diagnostic("\n  note: x\n"), "note: x")
        self.assertEqual(c23probe.diagnostic(""), "")

    def test_notes_list_only_probes_with_a_diagnostic(self) -> None:
        self.assertEqual(
            c23probe.render_notes([("a", "clang22", ""), ("b", "gcc15", "w: x")]),
            "diagnostics:\n  b / gcc15: w: x\n",
        )
        self.assertEqual(c23probe.render_notes([("a", "clang22", "")]), "")

    def test_run(self) -> None:
        self.assertEqual(c23probe.run_verdict("-5", "-5", ""), "ok")
        self.assertEqual(c23probe.run_verdict("-5", "-5", "w\n"), "ok (warns)")
        self.assertEqual(
            c23probe.run_verdict("-5", "w64d", ""), "no: printed 'w64d', want '-5'"
        )


class Table(unittest.TestCase):
    def test_render_aligns_columns(self) -> None:
        text = c23probe.render(
            ["clang22", "gcc15"],
            [("nullptr", ["yes", "yes"]), ("[[unsequenced]]", ["no fold", "folds"])],
        )
        self.assertEqual(
            text,
            "probe            clang22  gcc15\n"
            "nullptr          yes      yes\n"
            "[[unsequenced]]  no fold  folds\n",
        )


class Probes(unittest.TestCase):
    def test_names_unique_and_kinds_known(self) -> None:
        names = [p.name for p in c23probe.PROBES]
        self.assertEqual(len(names), len(set(names)))
        kinds = {c23probe.CALLS, c23probe.COMPILE, c23probe.RUN}
        for probe in c23probe.PROBES:
            self.assertIn(probe.kind, kinds, probe.name)

    def test_calls_probes_call_f_twice_in_source(self) -> None:
        for probe in c23probe.PROBES:
            if probe.kind == c23probe.CALLS:
                self.assertEqual(
                    probe.source.count("f(x)") + probe.source.count("f(p)"), 2
                )

    def test_run_probes_say_what_they_print(self) -> None:
        for probe in c23probe.PROBES:
            if probe.kind == c23probe.RUN:
                self.assertTrue(probe.expect, probe.name)

    def test_every_standard_attribute_has_its_gnu_baseline(self) -> None:
        names = {p.name for p in c23probe.PROBES}
        self.assertLessEqual({"gnu::const", "gnu::pure"}, names)
        self.assertLessEqual({"[[unsequenced]]", "[[reproducible]]"}, names)


if __name__ == "__main__":
    unittest.main()
