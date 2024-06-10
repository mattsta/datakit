project(sha1 C)
cmake_minimum_required(VERSION 3.5)

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


if(CMAKE_BUILD_TYPE MATCHES Debug)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0")
endif()

option(BuildTestBinary "Build test binary" On)

if(BuildTestBinary)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DDATAKIT_TEST")
endif()

add_library(sha1 OBJECT
    sha1.c)

#add_executable(sha1-test sha1.c)

add_library(sha1-shared  MODULE $<TARGET_OBJECTS:sha1>)
add_library(sha1-static  STATIC $<TARGET_OBJECTS:sha1>)
add_library(sha1-library SHARED $<TARGET_OBJECTS:sha1>)

set_target_properties(sha1-shared PROPERTIES PREFIX "") # don't prefix "lib"

set_target_properties(sha1-shared  PROPERTIES OUTPUT_NAME sha1)
set_target_properties(sha1-static  PROPERTIES OUTPUT_NAME sha1)
set_target_properties(sha1-library PROPERTIES OUTPUT_NAME sha1)

# SOVERSION only needs to increment when introducing *breaking* changes.
# Otherwise, just increase VERSION with normal feature additions or maint.
set_target_properties(sha1-library PROPERTIES VERSION 3.6 SOVERSION 3.6)


if(Build32Bit)
    set_target_properties(sha1
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(sha1-shared
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(sha1-static
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(sha1-library
                          PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()
# vi:ai et sw=4 ts=4:
