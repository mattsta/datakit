# memshard - datakit's high-throughput thread-caching allocator.
#
# Vendored from mimalloc (MIT License, see LICENSE) and being adapted to datakit
# conventions. POSIX only: macOS / Linux / BSD / Unix — no Windows.
#
# Source list mirrors the units compiled by mimalloc's amalgamation (src/static.c):
# files that are #include'd by others (alloc-override.c, page-queue.c,
# arena-abandon.c, prim/<os>/prim.c) are intentionally NOT listed here.

add_library(memshard STATIC
    src/alloc-aligned.c
    src/alloc-posix.c
    src/alloc.c        # includes alloc-override.c
    src/arena.c        # includes arena-abandon.c
    src/bitmap.c
    src/heap.c
    src/init.c
    src/libc.c
    src/options.c
    src/os.c
    src/page.c         # includes page-queue.c
    src/prim/prim.c    # includes the POSIX (unix/osx) primitive backend
    src/random.c
    src/segment-map.c
    src/segment.c
    src/stats.c
)

target_compile_definitions(memshard PRIVATE MI_STATIC_LIB)

target_include_directories(memshard
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

set_target_properties(memshard PROPERTIES POSITION_INDEPENDENT_CODE ON)

# --------------------------------------------------------------------------
# Header self-containment guard.
# The repo force-runs `form -i` (clang-format with include sorting), so no
# header may depend on include order. Compile each kept header ALONE in its own
# TU; a non-self-contained header fails this build. memshard depends on it, so
# the guard runs whenever memshard builds.
# --------------------------------------------------------------------------
set(_memshard_check_headers
    mimalloc.h
    mimalloc-stats.h
    mimalloc/types.h
    mimalloc/atomic.h
    mimalloc/internal.h
    mimalloc/prim.h
    mimalloc/track.h)
set(_memshard_check_srcs)
foreach(_h ${_memshard_check_headers})
    string(MAKE_C_IDENTIFIER "${_h}" _id)
    set(_stub "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck_${_id}.c")
    file(WRITE "${_stub}"
        "#include \"${_h}\"\nint memshard_hdrcheck_${_id}(void) { return 0; }\n")
    list(APPEND _memshard_check_srcs "${_stub}")
endforeach()
# bitmap.h is a private header resolved from src/.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck_bitmap_h.c"
    "#include \"bitmap.h\"\nint memshard_hdrcheck_bitmap_h(void) { return 0; }\n")
list(APPEND _memshard_check_srcs "${CMAKE_CURRENT_BINARY_DIR}/hdrcheck_bitmap_h.c")

add_library(memshard-headercheck OBJECT ${_memshard_check_srcs})
target_compile_definitions(memshard-headercheck PRIVATE MI_STATIC_LIB)
target_include_directories(memshard-headercheck PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_dependencies(memshard memshard-headercheck)
