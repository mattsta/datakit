#pragma once

#define COMPRESS_DEPTH(ptr) _PTRLIB_TOP_8_2(ptr)
#define COMPRESS_LIMIT(ptr) _PTRLIB_TOP_8_1(ptr)

#define SET_COMPRESS_DEPTH(ptr, depth) _PTRLIB_TOP_SET_8_2(ptr, depth)
#define SET_COMPRESS_LIMIT(ptr, limit) _PTRLIB_TOP_SET_8_1(ptr, limit)

#define SET_COMPRESS_DEPTH_LIMIT(ptr, depth, limit)                            \
    SET_COMPRESS_DEPTH(SET_COMPRESS_LIMIT(ptr, limit), depth)

#define RESET_COMPRESS_DEPTH(ptr, depth) _PTRLIB_TOP_RESET_8_2(ptr, depth)
#define RESET_COMPRESS_LIMIT(ptr, limit) _PTRLIB_TOP_RESET_8_1(ptr, limit)

#define REMOVE_META(ptr) _PTRLIB_TOP_USE(ptr)
