"""Which bytes of a C source are code.

A small lexer for tools that edit C by position: it tells comments,
string and character literals and (optionally) preprocessor lines
apart from code, so a tool can match parentheses or tokens without
being fooled by a ``)`` in a string or a name in a comment.
"""

from __future__ import annotations

_WORD = frozenset(b"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ_'.")


def _in_number(src: bytes, i: int) -> bool:
    """Whether the quote at 'i' is a digit separator: it follows a word
    that starts with a digit (a literal's prefix, such as L or u8, starts
    with a letter)."""
    start = i
    while start > 0 and src[start - 1] in _WORD:
        start -= 1
    return start < i and 0x30 <= src[start] <= 0x39


def code_mask(
    src: bytes, directives_are_code: bool = False, comments_are_code: bool = False
) -> bytearray:
    """1 for each byte of code; 0 for string and character literals and,
    unless 'comments_are_code', comments and, unless
    'directives_are_code', preprocessor lines (a directive runs to an
    unescaped newline)."""
    mask = bytearray(len(src))
    i = 0
    n = len(src)
    line_start = True
    while i < n:
        c = src[i : i + 1]
        if line_start and c in (b" ", b"\t"):
            mask[i] = 1
            i += 1
            continue
        if line_start and c == b"#" and not directives_are_code:
            while i < n and not (src[i] == 0x0A and src[i - 1] != 0x5C):
                i += 1
            continue
        line_start = False
        if src.startswith(b"/*", i):
            end = src.find(b"*/", i + 2)
            stop = n if end < 0 else end + 2
            if comments_are_code:
                mask[i:stop] = b"\x01" * (stop - i)
            i = stop
            continue
        if src.startswith(b"//", i):
            end = src.find(b"\n", i)
            stop = n if end < 0 else end
            if comments_are_code:
                mask[i:stop] = b"\x01" * (stop - i)
            i = stop
            continue
        if c == b"'" and _in_number(src, i):
            mask[i] = 1  # a digit separator (1'000'000)
            i += 1
            continue
        if c in (b'"', b"'"):
            # a literal ends at its quote, or (unterminated, as an
            # apostrophe in #error text) at the end of its line
            i += 1
            while i < n and src[i : i + 1] not in (c, b"\n"):
                i += 2 if src[i] == 0x5C else 1
            if i < n and src[i : i + 1] == c:
                i += 1
            continue
        mask[i] = 1
        if c == b"\n":
            line_start = True
        i += 1
    return mask
