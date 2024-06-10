cmake_minimum_required(VERSION 3.5)

project(xxhash C)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if (TARGET xxhash)
    return()
endif(TARGET xxhash)

if(APPLE)
    set(CMAKE_SHARED_MODULE_CREATE_C_FLAGS
        "${CMAKE_SHARED_MODULE_CREATE_C_FLAGS} -undefined dynamic_lookup")
    cmake_policy(SET CMP0042 NEW)
endif()

option(Build32Bit "Build 32-bit Library" OFF)

# If you need debug, build with:
# cmake -DCMAKE_BUILD_TYPE=Debug ..
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -Wextra -pedantic -std=c99 -Wno-extra-semi -Wno-missing-field-initializers")

add_library(xxhash OBJECT
    xxhash.c)

add_definitions(
    # Turn xxhash into a static header-only library
    # Note: can't build xxhash.c with this option.
    #    -DXXH_INLINE_ALL

    # 0 = crash on null pointer
    # 1 = null pointers return 0 value hash
    -DXXH_ACCEPT_NULL_INPUT_POINTER=0

    # 0 = memcpy
    # 1 = __packed__
    # 2 = unaligned reads
    -DXXH_FORCE_MEMORY_ACCESS=0
    # Types 1 and 2 cause unaligned warnings from ubsan
    # Benchmark types 0, 1, and 2 and pick the best regardless
    # of warnings!
)

add_library(xxhash-shared  MODULE $<TARGET_OBJECTS:xxhash>)
add_library(xxhash-static  STATIC $<TARGET_OBJECTS:xxhash>)
add_library(xxhash-library SHARED $<TARGET_OBJECTS:xxhash>)

set_target_properties(xxhash-shared PROPERTIES PREFIX "") # don't prefix "lib"

set_target_properties(xxhash-shared  PROPERTIES OUTPUT_NAME xxhash)
set_target_properties(xxhash-static  PROPERTIES OUTPUT_NAME xxhash)
set_target_properties(xxhash-library PROPERTIES OUTPUT_NAME xxhash)

# SOVERSION only needs to increment when introducing *breaking* changes.
# Otherwise, just increase VERSION with normal feature additions or maint.
set_target_properties(xxhash-library PROPERTIES VERSION 3.6 SOVERSION 3.6)

if(Build32Bit)
    set_target_properties(xxhash
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(xxhash-shared
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(xxhash-static
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(xxhash-library
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()
# vi:ai et sw=4 ts=4:
