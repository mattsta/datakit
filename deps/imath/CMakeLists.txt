cmake_minimum_required(VERSION 3.10)
project(imath C)

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

add_library(imath OBJECT
    imath.c)

add_library(imath-shared  MODULE $<TARGET_OBJECTS:imath>)
add_library(imath-static  STATIC $<TARGET_OBJECTS:imath>)
add_library(imath-library SHARED $<TARGET_OBJECTS:imath>)

set_target_properties(imath-shared PROPERTIES PREFIX "") # don't prefix "lib"

set_target_properties(imath-shared  PROPERTIES OUTPUT_NAME imath)
set_target_properties(imath-static  PROPERTIES OUTPUT_NAME imath)
set_target_properties(imath-library PROPERTIES OUTPUT_NAME imath)

# SOVERSION only needs to increment when introducing *breaking* changes.
# Otherwise, just increase VERSION with normal feature additions or maint.
set_target_properties(imath-library PROPERTIES VERSION 3.6 SOVERSION 3.6)

add_executable(pi-${PROJECT_NAME} pi.c)
target_link_libraries(pi-${PROJECT_NAME} imath-static)

# imath-edge: canary-bounded checks for imath's string writers and the
# gmp_compat layer. It compiles imrat.c and gmp_compat.c, which the
# libraries above do not, so they are built and tested on every build.
# It uses its own sources rather than the imath object library so the
# sanitizers apply to all of the code under test.
add_executable(imath-edge-test
    tests/imathEdgeTest.c
    imath.c
    imrat.c
    gmp_compat.c)
target_include_directories(imath-edge-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
if(EnableASAN)
    target_compile_options(imath-edge-test PRIVATE
        -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(imath-edge-test PRIVATE -fsanitize=address)
endif()
if(EnableUBSAN)
    target_compile_options(imath-edge-test PRIVATE
        -fsanitize=undefined -fno-sanitize-recover=undefined)
    target_link_options(imath-edge-test PRIVATE -fsanitize=undefined)
endif()
add_test(NAME imath-edge COMMAND imath-edge-test)

if(Build32Bit)
    set_target_properties(imath
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(imath-shared
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(imath-static
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(imath-library
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()
# vi:ai et sw=4 ts=4:
