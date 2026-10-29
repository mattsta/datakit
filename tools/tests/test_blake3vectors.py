"""``dktools blake3vectors``: the vectors' JSON checked and rendered, and
the header compared or rewritten.

Run from the repository root::

    uv run --project tools python -m unittest discover -s tools/tests
"""

from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path

from dktools import blake3vectors
from dktools.gitutil import ToolError

KEY = "whats the Elvish word for friend"
CONTEXT = "BLAKE3 2019-12-27 16:29:52 test vectors context"


def vectors(cases: list[dict] | None = None, key: str = KEY) -> str:
    return json.dumps(
        {
            "key": key,
            "context_string": CONTEXT,
            "cases": cases
            if cases is not None
            else [
                {
                    "input_len": 0,
                    "hash": "aa" * 70,
                    "keyed_hash": "bb" * 70,
                    "derive_key": "cc" * 70,
                },
                {
                    "input_len": 1,
                    "hash": "dd" * 70,
                    "keyed_hash": "ee" * 70,
                    "derive_key": "ff" * 70,
                },
            ],
        }
    )


class Load(unittest.TestCase):
    def test_a_well_formed_file_loads(self) -> None:
        data = blake3vectors.load(vectors())
        self.assertEqual(len(data["cases"]), 2)

    def test_malformed_files_are_refused(self) -> None:
        bad = [
            "not json",
            json.dumps([1, 2]),
            vectors(key="short"),
            vectors(cases=[]),
            vectors(
                cases=[
                    {
                        "input_len": 0,
                        "hash": "zz",
                        "keyed_hash": "aa",
                        "derive_key": "aa",
                    }
                ]
            ),
            vectors(
                cases=[
                    {
                        "input_len": 0,
                        "hash": "aa",
                        "keyed_hash": "aabb",
                        "derive_key": "aa",
                    }
                ]
            ),
            vectors(cases=[{"hash": "aa", "keyed_hash": "aa", "derive_key": "aa"}]),
        ]
        for text in bad:
            with self.subTest(text=text[:40]), self.assertRaises(ToolError):
                blake3vectors.load(text)


class Render(unittest.TestCase):
    def test_every_output_whole_in_64_character_literals(self) -> None:
        text = blake3vectors.render(blake3vectors.load(vectors()))
        self.assertIn("#define BLAKE3_TEST_OUTPUT_LEN 70", text)
        self.assertIn(f'#define BLAKE3_TEST_KEYED_KEY "{KEY}"', text)
        self.assertIn(f'#define BLAKE3_TEST_DERIVE_CONTEXT "{CONTEXT}"', text)
        # 140 hex characters: two full literals and a 12-character tail
        self.assertIn(
            '     "'
            + "aa" * 32
            + '"\n     "'
            + "aa" * 32
            + '"\n     "'
            + "aa" * 6
            + '",',
            text,
        )
        self.assertIn('"' + "ff" * 6 + '"},', text)
        self.assertTrue(text.startswith("#pragma once\n"))
        self.assertIn("/* clang-format off */", text)
        self.assertTrue(text.endswith("/* clang-format on */\n"))


class Run(unittest.TestCase):
    def test_check_then_write_then_check(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            (repo / "src" / "hash").mkdir(parents=True)
            source = repo / "test_vectors.json"
            source.write_text(vectors())
            self.assertEqual(blake3vectors.run(repo, source, write=False), 1)
            self.assertEqual(blake3vectors.run(repo, source, write=True), 0)
            self.assertEqual(blake3vectors.run(repo, source, write=False), 0)
            (repo / blake3vectors.HEADER).write_text("edited\n")
            self.assertEqual(blake3vectors.run(repo, source, write=False), 1)


if __name__ == "__main__":
    unittest.main()
