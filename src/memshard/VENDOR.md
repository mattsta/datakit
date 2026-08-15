# memshard vendor import & refresh discipline

memshard is vendored from mimalloc (MIT, see LICENSE) and then **taken
over**: we import upstream changes into OUR adapted implementation and
improve them to house standards. We never wholesale-copy an upstream
tree over the adapted one, and we never let upstream's conventions
override ours.

Current base: mimalloc v2.3.2 with the v2.4.x correctness line
integrated by hand (see git history for the per-change record).

## The upstream reference

## What we change on every import (the house adaptation)

1. **POSIX only — no Microsoft anything.** Delete every branch guarded
   by `_MSC_VER`, `_WIN32`, `WIN32`, `__CYGWIN__`, `__MINGW32__`/`64__`,
   `_M_X64`, `_M_IX86`, `_M_ARM64`, `__wasi__`, `__EMSCRIPTEN__`,
   keeping the non-Windows side byte-for-byte. Windows/wasi/emscripten
   prim backends are not vendored at all. Keep `__APPLE__`, `__linux__`,
   BSD, and `__sun` branches.
2. **House formatting applies to vendored code too.** Everything runs
   through `form -i` (clang-format, LLVM base, IndentWidth 4,
   InsertBraces). Because InsertBraces mangles mimalloc's paren-less
   `if mi_likely(x)` / `while mi_unlikely(x)` idiom, rewrite those to
   the parenthesized form `if (mi_likely(x))` BEFORE formatting.
3. **Headers must be self-contained.** `form -i` sorts includes, so no
   header may depend on include order; each header includes its own
   prerequisites. Enforced at build time by the `memshard-headercheck`
   target (every kept header compiles alone in its own TU).
4. **Integrate by hand, per change.** Diff the upstream base tag
   against the target tag, read every hunk, and hand-apply the ones we
   want at the matching place in our files. Upstream hunks never apply
   mechanically (formatting differs). No `sed`/`perl` in-place, no
   `unifdef` — deliberate edits only.
5. **Take correctness, skip churn.** Adopt bug/security/robustness
   fixes (bounds, overflow, UB, race, fail-closed). Skip Windows-only
   changes, version-number bumps, whitespace, and API refactors we
   don't need — implement the underlying fix inside our existing
   signatures instead of adopting upstream's plumbing.
6. **Minimal API surface.** Built as `MI_STATIC_LIB`; the
   malloc-override machinery (`alloc-override*.c`, the macOS zone
   interposer, `mimalloc-override.h`, `mimalloc-new-delete.h`) is not
   vendored. datakit reaches memshard only through the explicit
   `datakitUseMemshard()` adapter (`src/memshardConfig.c`).
7. **Compilation units mirror the amalgamation.** Files that upstream
   `#include`s into another TU (`page-queue.c` in `page.c`,
   `arena-abandon.c` in `arena.c`, `prim/unix|osx/prim.c` in
   `prim/prim.c`) are not listed as sources — see
   `src/memshard/CMakeLists.txt`.
8. **Comments:** upstream issue references ("issue #763") stay as
   provenance; no local absolute paths; no project-management IDs.

## Verification for any vendor change

- Build `memshard`, `memshardTest`, `memshardConfigTest` under the
  `dev` preset (ASAN/UBSAN) and run both test binaries.
- Benchmarks (`memboundBench`) only under the `perf` preset.
