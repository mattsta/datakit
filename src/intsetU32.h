#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* STABLE OUTER HANDLE.
 *
 * The handle (`intsetU32 *`) is a fixed-size header; the element buffer
 * lives in a SEPARATELY heap-allocated block reached through `contents`.
 * Growth and shrink (Add / Remove / Resize / ShrinkToSize / RandomDelete)
 * reallocate `contents` IN PLACE and NEVER relocate the handle itself.
 *
 * INVARIANT: a pointer to an intsetU32 stays valid across any in-place
 * mutation.  A holder of the handle (a value slot in a keyspace, a databox
 * that captured it, a persist snapshot that reads it later on another
 * thread) may therefore keep the SAME pointer across mutations — the buffer
 * moves underneath the handle, but the handle does not.  This is the same
 * stable-outer-handle contract intsetBig exposes (its {multimap*;count}
 * header never moves while its buckets grow).
 *
 * Because the handle never moves, the mutators take `intsetU32 *` (not
 * `intsetU32 **`): there is no new handle to hand back. */
typedef struct intsetU32 {
    uint32_t count;
    uint32_t capacity; /* slots allocated in 'contents' (>= count) */
    uint32_t *contents;
} intsetU32;

intsetU32 *intsetU32New(void);
intsetU32 *intsetU32NewLen(const uint32_t len);
intsetU32 *intsetU32Copy(const intsetU32 *is);
uint32_t *intsetU32Array(intsetU32 *is);
void intsetU32Resize(intsetU32 *is, uint32_t len);
void intsetU32ShrinkToSize(intsetU32 *is);
void intsetU32UpdateCount(intsetU32 *is, uint32_t len);
void intsetU32Free(intsetU32 *is);
bool intsetU32Add(intsetU32 *is, uint32_t value);
bool intsetU32Remove(intsetU32 *is, uint32_t value);
uint32_t intsetU32Merge(intsetU32 *dst, const intsetU32 *b);
bool intsetU32Exists(const intsetU32 *is, uint32_t value);
bool intsetU32Equal(const intsetU32 *a, const intsetU32 *b);
bool intsetU32Subset(const intsetU32 *a, const intsetU32 *b);
bool intsetU32Get(const intsetU32 *is, uint32_t pos, uint32_t *value);
uint32_t intsetU32Random(intsetU32 *is);
bool intsetU32RandomDelete(intsetU32 *is, uint32_t *deleted);
size_t intsetU32Count(const intsetU32 *is);
size_t intsetU32Bytes(const intsetU32 *is);

#ifdef DATAKIT_TEST
int intsetU32Test(int argc, char *argv[]);
void intsetU32Repr(const intsetU32 *is);
#endif

/*
 * Copyright (c) 2009-2012, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
