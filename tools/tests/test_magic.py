import subprocess
import tempfile
import unittest
from pathlib import Path

from dktools import magic
from dktools.gitutil import ToolError


def _scan(files: dict[str, str]) -> list[magic.Hit]:
    return magic.scan_files({p: s.encode() for p, s in files.items()})


class RepeatedLimitTests(unittest.TestCase):
    def test_a_limit_refused_in_two_files_is_flagged_in_both(self):
        hits = _scan(
            {
                "src/a.c": "bool f(int level) {\n"
                "    if (level > 12) {\n"
                "        return false;\n"
                "    }\n"
                "    return true;\n"
                "}\n",
                "src/b.c": "void *g(const uint64_t *v) {\n"
                "    if (v[4] > 12 || v[1] == 0) return nullptr;\n"
                "    return v;\n"
                "}\n",
            }
        )
        self.assertEqual(
            [(h.path, h.line, h.rule, h.key) for h in hits],
            [
                ("src/a.c", 2, magic.REPEATED_LIMIT, "12"),
                ("src/b.c", 2, magic.REPEATED_LIMIT, "12"),
            ],
        )

    def test_a_refusal_that_cleans_up_first_and_a_clamp_count(self):
        hits = _scan(
            {
                "src/a.c": "void *f(flex *rows, int level) {\n"
                "    if (level > 12) {\n"
                "        flexFree(rows);\n"
                "        return nullptr;\n"
                "    }\n"
                "    return rows;\n"
                "}\n",
                "src/b.c": "void g(int *levels) {\n"
                "    if (*levels > 12) {\n"
                "        *levels = 12;\n"
                "    }\n"
                "}\n",
            }
        )
        self.assertEqual(
            [(h.path, h.line) for h in hits], [("src/a.c", 2), ("src/b.c", 2)]
        )

    def test_a_limit_checked_twice_in_one_file_is_flagged(self):
        hits = _scan(
            {
                "src/a.c": "bool w(size_t room) { if (room < 1024) return false; return true; }\n"
                "void *r(size_t room) { if (room < 1024) return nullptr; return 0; }\n"
            }
        )
        self.assertEqual([(h.line, h.key) for h in hits], [(1, "1024"), (2, "1024")])

    def test_one_check_small_numbers_and_non_refusals_pass(self):
        hits = _scan(
            {
                # Checked once: its own rule, said once
                "src/a.c": "int f(int x) { if (x > 99) return -1; return 0; }\n",
                # Below 10: lengths and counts, not shared limits
                "src/b.c": "int g(int n) { if (n < 2) return 0; return n; }\n"
                "int h(int n) { if (n < 2) return 1; return n; }\n",
                "src/c.c": "int k(int n) { if (n < 2) return 0; return n; }\n"
                # Not a refusal: the if does work instead of returning
                "void m(int x) { if (x > 99) { x = 0; } }\n",
            }
        )
        self.assertEqual(hits, [])

    def test_comments_literals_and_test_sections_do_not_count(self):
        hits = _scan(
            {
                "src/a.c": "/* if (x > 64) return 0; */\n"
                'const char *s = "if (x > 64) return 0;";\n'
                "#ifdef DATAKIT_TEST\n"
                "int t(int x) { if (x > 64) return 1; return 0; }\n"
                "#endif\n",
                "src/b.c": "int f(int x) { if (x > 64) return 1; return 0; }\n",
            }
        )
        self.assertEqual(hits, [])

    def test_hex_suffixes_and_reversed_operands_group_by_value(self):
        hits = _scan(
            {
                "src/a.c": "int f(unsigned x) { if (x >= 0x40u) return 1; return 0; }\n",
                "src/b.c": "int g(unsigned x) { if (64 <= x) return 1; return 0; }\n",
            }
        )
        self.assertEqual([h.key for h in hits], ["64", "64"])


class PositionalFieldTests(unittest.TestCase):
    def test_three_positions_of_one_array_in_a_restore_are_flagged_once(self):
        hits = _scan(
            {
                "src/a.c": "static void *fooRestore_(const uint8_t *d) {\n"
                "    uint64_t v[4];\n"
                "    if (v[1] > 3) return nullptr;\n"
                "    use(v[0], v[2]);\n"
                "    return d;\n"
                "}\n"
            }
        )
        self.assertEqual(
            [(h.line, h.rule, h.key) for h in hits],
            [(3, magic.POSITIONAL, "fooRestore_:v")],
        )

    def test_other_functions_declarations_and_two_positions_pass(self):
        hits = _scan(
            {
                "src/a.c": "static int sum(const int *v) { return v[0] + v[1] + v[2]; }\n"
                "static void barSnapshot_(uint64_t *out) {\n"
                "    uint64_t fields[8] = {};\n"
                "    out[0] = fields[0];\n"
                "    out[1] = fields[1];\n"
                "}\n"
            }
        )
        self.assertEqual(hits, [])


class AllowFileTests(unittest.TestCase):
    def _repo(self, files: dict[str, str], allow: str) -> Path:
        root = Path(tempfile.mkdtemp())
        for path, text in files.items():
            (root / path).parent.mkdir(parents=True, exist_ok=True)
            (root / path).write_text(text)
        (root / "tools").mkdir(exist_ok=True)
        (root / magic.ALLOW_FILE).write_text(allow)
        subprocess.run(["git", "init", "-q"], cwd=root, check=True)
        subprocess.run(["git", "add", "-A"], cwd=root, check=True)
        return root

    LIMITS = {
        "src/a.c": "int f(int x) { if (x > 64) return 1; return 0; }\n",
        "src/b.c": "int g(int x) { if (x > 64) return 1; return 0; }\n",
    }

    def test_allowed_hits_pass_and_unallowed_fail(self):
        repo = self._repo(
            self.LIMITS, "src/a.c repeated-limit 64 a different rule, by chance\n"
        )
        self.assertEqual(magic.run(repo, ()), 1)
        repo = self._repo(
            self.LIMITS,
            "src/a.c repeated-limit 64 a different rule, by chance\n"
            "src/b.c repeated-limit 64 a different rule, by chance\n",
        )
        self.assertEqual(magic.run(repo, ()), 0)

    def test_a_star_allows_every_hit_of_the_rule_in_its_file(self):
        repo = self._repo(
            self.LIMITS,
            "src/a.c repeated-limit * encoding facts, as the table writes them\n"
            "src/b.c repeated-limit 64 reason\n",
        )
        self.assertEqual(magic.run(repo, ()), 0)
        repo = self._repo(
            self.LIMITS,
            "src/a.c repeated-limit 64 reason\n"
            "src/b.c repeated-limit 64 reason\n"
            "src/b.c positional-fields * nothing positional there\n",
        )
        self.assertEqual(magic.run(repo, ()), 1)

    def test_a_stale_allow_line_fails(self):
        repo = self._repo(
            self.LIMITS,
            "src/a.c repeated-limit 64 reason\n"
            "src/b.c repeated-limit 64 reason\n"
            "src/b.c repeated-limit 99 no longer there\n",
        )
        self.assertEqual(magic.run(repo, ()), 1)

    def test_an_allow_line_needs_a_known_rule_and_a_reason(self):
        with self.assertRaises(ToolError):
            magic.parse_allow("src/a.c repeated-limit 64\n")
        with self.assertRaises(ToolError):
            magic.parse_allow("src/a.c no-such-rule 64 reason\n")


if __name__ == "__main__":
    unittest.main()
