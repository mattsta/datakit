cmake_minimum_required(VERSION 3.10)
project(datakit C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_DISABLE_IN_SOURCE_BUILD ON)
set(CMAKE_DISABLE_SOURCE_CHANGES  ON)

if (TARGET ${PROJECT_NAME})
    return()
endif()

# Enable testing at the top level so tests are properly registered
enable_testing()

add_subdirectory(deps)
add_subdirectory(src)

# Allocator benchmark: membound vs memshard vs system malloc. Build with
#   cmake --build <dir> --target memboundBench
find_package(Threads REQUIRED)
add_executable(memboundBench
    ${CMAKE_CURRENT_SOURCE_DIR}/src/memboundBench.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/memshardConfig.c)
target_include_directories(memboundBench PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/memshard/include)
target_link_libraries(memboundBench PRIVATE datakit-static memshard Threads::Threads)

# Decision microbench for branchy vs branchless+prefetch binary search.
# Self-contained — no datakit deps. Build: cmake --build <dir> --target intsetSearchBench
add_executable(intsetSearchBench ${CMAKE_CURRENT_SOURCE_DIR}/src/intsetSearchBench.c)

# memshard correctness tests (run under the `dev` preset for ASAN/UBSAN):
#   cmake --build <dir> --target memshardTest && ./<dir>/memshardTest
add_executable(memshardTest ${CMAKE_CURRENT_SOURCE_DIR}/src/memshardTest.c)
target_include_directories(memshardTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/memshard/include)
target_link_libraries(memshardTest PRIVATE memshard Threads::Threads)

# memshard-as-datakit-allocator adapter + its validation (opt-in integration:
# routes datakit's global z* allocator through memshard).
add_executable(memshardConfigTest
    ${CMAKE_CURRENT_SOURCE_DIR}/src/memshardConfigTest.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/memshardConfig.c)
target_include_directories(memshardConfigTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/memshard/include)
target_link_libraries(memshardConfigTest PRIVATE datakit-static memshard Threads::Threads)
# vi:ai et sw=4 ts=4:
