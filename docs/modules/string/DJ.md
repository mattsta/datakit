# dj: streaming JSON writer

**Header**: `dj.h` **Source**: `dj.c`

dj writes JSON text into `mds` buffers as you call it, one element at a time: no document tree is built
and nothing is parsed. It tracks only the open containers (map, array or set) and whether the next element
in a map is a key or a value, so it writes every separator for you.

## Writing a document

```c
#include "dj.h"

djState st = {};
djInit(&st);

djMapOpen(&st);
djStringDirect(&st, "name", 4);           /* key */
djString(&st, userName, nameLen, true);   /* value, escaped */
djStringDirect(&st, "tags", 4);
djArrayOpen(&st);
djStringDirect(&st, "a", 1);
djStringDirect(&st, "b", 1);
djArrayCloseElement(&st);
djStringDirect(&st, "age", 3);
djNumericDirect(&st, "42", 2);
djStringDirect(&st, "admin", 5);
djFalse(&st);
djMapCloseFinal(&st);

mds *json = djFinalize(&st); /* {"name":"...","tags":["a","b"],"age":42,"admin":false} */
/* ... */
mdsfree(json);
```

The rules:

- **Keys and values alternate inside a map.** Element 1 is a key, element 2 its value, and so on. A
  container opened in value position is that key's value.
- **Close nested containers with `…CloseElement`** (`djMapCloseElement`, `djArrayCloseElement`,
  `djSetCloseElement`): it writes the bracket followed by a separator, so more elements may follow. Close
  with `…CloseFinal` (`djMapCloseFinal`, `djArrayCloseFinal`, `djSetCloseFinal`) only when nothing
  follows at that level: it writes no separator. `djFinalize`
  removes a trailing separator, so closing everything with `…CloseElement` is also correct.
  `djCloseElement` closes whichever kind is innermost.
- **`djFinalize`** returns the document (the caller frees it with `mdsfree`) and leaves the state zeroed,
  ready for `djInit` again; `djFree` discards an unfinished one.

## Values

| Call                            | Writes                                                                   |
| ------------------------------- | ------------------------------------------------------------------------ |
| `djString(&st, p, len, true)`   | a quoted, escaped string (`"`, `\` and control bytes escaped)            |
| `djStringDirect(&st, p, len)`   | a quoted string copied as is: for text you know needs no escaping        |
| `djNumericDirect(&st, p, len)`  | bytes copied unquoted (a number you formatted); quoted when it is a key  |
| `djTrue` / `djFalse` / `djNULL` | `true` / `false` / `null`                                                |
| `djBox(&st, &box)`              | a databox: numbers, escaped bytes, true/false/null (false: no JSON form) |

`djString` copies bytes from 0x80 up as they are, so valid UTF-8 input gives valid JSON; its last argument
does not change the output. `djBox` refuses (writes nothing, returns false) what JSON cannot hold: a
non-finite float, VOID, ERROR, a container or a reference; in key position it writes every scalar as a
string key.

`djSetOpen` / `djSetCloseElement` / `djSetCloseFinal` write a Python-style set, `{1,2,3}`: braces holding
values without keys. It is not JSON; use it for Python-readable output.

## Streaming out large output

`djConsumeBuffer(&st, true)` takes the text written so far and lets writing continue into a fresh buffer.
The chunks taken, followed by what `djFinalize` returns, are the document, so a large one can be sent as it
is written:

```c
for (size_t i = 0; i < rowCount; i++) {
    writeRow(&st, &rows[i]);
    if (mdslen(st.s) > 64 * 1024) {
        mds *chunk = djConsumeBuffer(&st, true);
        sendAll(fd, chunk, mdslen(chunk));
        mdsfree(chunk);
    }
}
mds *tail = djFinalize(&st);
```

## Building parts separately

Parts written into separate states (for example by worker threads, one state each) join without
copying: `djAppend(&dst, &src)` moves every buffer of `src` to the end of `dst`, and `dst` keeps
writing after them. The output then spans several buffers:

- `djGetMulti(&st, &count)` is a view of them in order (still owned by the state);
- `djFinalizeMulti(&st, &count)` hands them over: the caller frees the array with `zfree` and each buffer
  with `mdsfree`. Write them with one `writev`.

`djInitWithBuffer(&st, buf)` starts a state on an `mds` you supply: the state owns it from then, keeps its
contents and writes after them.

## Cautions

- A state is single-threaded. Separate states are independent.
- dj does not validate structure: a key without its value, or a close of the wrong kind, writes broken
  JSON (debug builds assert on a close with no container open).
- `djStringDirect` and `djNumericDirect` copy bytes unchecked; anything from outside goes through
  `djString` or `djBox`.
