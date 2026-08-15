#pragma once

/* memshard ↔ datakit allocator adapter.
 *
 * Opt-in: lets a consumer route datakit's global z* allocator (the
 * `datakitConfig` function-pointer struct used by zmalloc/zcalloc/zrealloc/
 * zfree/zmemalign) through the memshard allocator instead of libc/jemalloc.
 *
 * datakit core does NOT depend on memshard; this adapter is built/linked only
 * by consumers that want memshard as the global allocator. Link both
 * `datakit-static` and `memshard` plus this unit, then call
 * datakitUseMemshard() (or pass datakitMemshardConfig() to datakitConfigSet).
 */

#include "datakit.h" /* datakitConfig */

/* Build a datakitConfig whose z* operations are backed by memshard (mi_*). */
datakitConfig datakitMemshardConfig(void);

/* Convenience: install the memshard-backed config as the process global.
 * Returns true on success. Call once, before any z* allocation you want routed
 * through memshard. */
bool datakitUseMemshard(void);
