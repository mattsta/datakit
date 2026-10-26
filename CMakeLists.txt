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

find_package(Threads REQUIRED)

add_subdirectory(deps)
add_subdirectory(src)

# Decision microbench for branchy vs branchless+prefetch binary search.
# Self-contained — no datakit deps. Build: cmake --build <dir> --target intsetSearchBench
add_executable(intsetSearchBench ${CMAKE_CURRENT_SOURCE_DIR}/src/intsetSearchBench.c)
# Every datakit source is C23, as src/CMakeLists.txt builds its own
target_compile_options(intsetSearchBench PRIVATE -std=gnu23)

# memshard correctness tests (run under the `dev` preset for ASAN/UBSAN):
#   cmake --build <dir> --target memshardTest && ./<dir>/memshardTest
add_executable(memshardTest ${CMAKE_CURRENT_SOURCE_DIR}/src/memshardTest.c)
target_include_directories(memshardTest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/memshard/include)
target_link_libraries(memshardTest PRIVATE memshard Threads::Threads)
target_compile_options(memshardTest PRIVATE -std=gnu23)
# vi:ai et sw=4 ts=4:
