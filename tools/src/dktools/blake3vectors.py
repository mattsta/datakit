"""The BLAKE3 test vector header from the canonical test_vectors.json.

Usage::

    dktools blake3vectors JSON [--write]

JSON is the BLAKE3 team's ``test_vectors/test_vectors.json`` (fetched by
the caller, e.g. from
https://raw.githubusercontent.com/BLAKE3-team/BLAKE3/master/test_vectors/test_vectors.json).
The header ``src/hash/blake3TestVectors.h`` holds every case's input
length and its three extended outputs (plain hash, keyed hash, derive
key) whole, as hex.  Without ``--write`` the command checks that the
header in the repository is exactly what the JSON renders (exit 0 when
it is, 1 when it differs); ``--write`` rewrites it.  Exit 2 on an input
error (a file that is not the vectors' JSON).
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

from .gitutil import ToolError

#: Where the header lives
HEADER = Path("src/hash/blake3TestVectors.h")

_HEX = re.compile(r"^[0-9a-f]+$")


def load(text: str, source: str = "JSON") -> dict:
    """The parsed vectors; ToolError unless every case has an input length
    and three hex outputs of one length, with the key and context"""
    try:
        data = json.loads(text)
    except ValueError as exc:
        raise ToolError(f"{source}: not JSON ({exc})") from None
    if not isinstance(data, dict):
        raise ToolError(f"{source}: not the test vectors' JSON object")
    key = data.get("key")
    context = data.get("context_string")
    cases = data.get("cases")
    if not isinstance(key, str) or len(key) != 32 or not isinstance(context, str):
        raise ToolError(f"{source}: no 32-byte key or no context string")
    if not isinstance(cases, list) or not cases:
        raise ToolError(f"{source}: no cases")
    width = None
    for n, case in enumerate(cases):
        fields = ("hash", "keyed_hash", "derive_key")
        if not isinstance(case, dict) or not isinstance(case.get("input_len"), int):
            raise ToolError(f"{source}: case {n} has no input length")
        for f in fields:
            value = case.get(f)
            if not isinstance(value, str) or not _HEX.match(value) or len(value) % 2:
                raise ToolError(f"{source}: case {n} {f} is not hex")
            if width is None:
                width = len(value)
            elif len(value) != width:
                raise ToolError(f"{source}: case {n} {f} is not {width // 2} bytes")
    return data


def _hex_lines(value: str, indent: str) -> list[str]:
    """'value' as adjacent string literals of 64 hex characters"""
    return [f'{indent}"{value[i : i + 64]}"' for i in range(0, len(value), 64)]


def render(data: dict) -> str:
    """The header text for the parsed vectors"""
    cases = data["cases"]
    out_len = len(cases[0]["hash"]) // 2
    lines = [
        "#pragma once",
        "",
        "/* BLAKE3 test vectors: the canonical test_vectors.json, every case's",
        f" * extended outputs whole ({out_len} bytes).  Written by `dktools",
        " * blake3vectors JSON --write` (tools/README.md), which checks the file",
        " * against the JSON without --write; do not edit by hand.",
        " *",
        " * Input pattern: byte i of input == (i % 251).  Keyed-hash key is the",
        f' * 32-byte ASCII string "{data["key"]}".  Derive-key',
        f' * context string is "{data["context_string"]}".',
        " *",
        " * Each entry's three hex fields are: hash (plain), keyed_hash, derive_key.",
        " */",
        "",
        "/* clang-format off */",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        f'#define BLAKE3_TEST_KEYED_KEY "{data["key"]}"',
        f'#define BLAKE3_TEST_DERIVE_CONTEXT "{data["context_string"]}"',
        "/* The bytes each hex field holds */",
        f"#define BLAKE3_TEST_OUTPUT_LEN {out_len}",
        "",
        "typedef struct blake3TestVector {",
        "    size_t input_len;",
        "    const char *hash_hex;",
        "    const char *keyed_hash_hex;",
        "    const char *derive_key_hex;",
        "} blake3TestVector;",
        "",
        "static const blake3TestVector blake3TestVectors[] = {",
    ]
    for case in cases:
        lines.append(f"    {{{case['input_len']},")
        for i, field in enumerate(("hash", "keyed_hash", "derive_key")):
            literal = _hex_lines(case[field], "     ")
            literal[-1] += "," if i < 2 else "},"
            lines.extend(literal)
    lines += [
        "};",
        "",
        "#define BLAKE3_TEST_VECTOR_COUNT (countof(blake3TestVectors))",
        "/* clang-format on */",
        "",
    ]
    return "\n".join(lines)


def run(repo: Path, json_path: Path, write: bool) -> int:
    """Check (or with 'write', rewrite) the header; the exit code"""
    try:
        text = json_path.read_text(encoding="utf-8")
    except OSError as exc:
        raise ToolError(f"{json_path}: {exc}") from None
    rendered = render(load(text, str(json_path)))
    header = repo / HEADER
    if write:
        header.write_text(rendered)
        sys.stdout.write(f"blake3vectors: wrote {HEADER}\n")
        return 0
    current = header.read_text(encoding="utf-8") if header.is_file() else ""
    if current == rendered:
        sys.stdout.write(f"blake3vectors: {HEADER} matches {json_path}\n")
        return 0
    sys.stdout.write(f"blake3vectors: {HEADER} differs from {json_path}\n")
    return 1
