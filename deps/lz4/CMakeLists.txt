cmake_minimum_required(VERSION 3.10)
project(lz4 C)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(APPLE)
    set(CMAKE_SHARED_MODULE_CREATE_C_FLAGS
        "${CMAKE_SHARED_MODULE_CREATE_C_FLAGS} -undefined dynamic_lookup")
    cmake_policy(SET CMP0042 NEW)
endif()

option(Build32Bit "Build 32-bit Library" OFF)

# If you need debug, build with:
# cmake -DCMAKE_BUILD_TYPE=Debug ..
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O2 -Wall -Wextra -pedantic -std=c99 -Wno-missing-field-initializers")

add_library(lz4 OBJECT
    lz4.c
    lz4frame.c
    lz4hc.c
    lz4frame.c)

add_library(lz4-shared  MODULE $<TARGET_OBJECTS:lz4>)
add_library(lz4-static  STATIC $<TARGET_OBJECTS:lz4>)
add_library(lz4-library SHARED $<TARGET_OBJECTS:lz4>)

set_target_properties(lz4-shared PROPERTIES PREFIX "") # don't prefix "lib"

set_target_properties(lz4-shared  PROPERTIES OUTPUT_NAME lz4)
set_target_properties(lz4-static  PROPERTIES OUTPUT_NAME lz4)
set_target_properties(lz4-library PROPERTIES OUTPUT_NAME lz4)

target_link_libraries(lz4-static xxhash-static)
target_link_libraries(lz4-library xxhash-library)

# SOVERSION only needs to increment when introducing *breaking* changes.
# Otherwise, just increase VERSION with normal feature additions or maint.
set_target_properties(lz4-library PROPERTIES VERSION 3.6 SOVERSION 3.6)

if(Build32Bit)
    set_target_properties(lz4
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(lz4-shared
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(lz4-static
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(lz4-library
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()
# vi:ai et sw=4 ts=4:
