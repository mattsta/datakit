cmake_minimum_required(VERSION 3.10...3.28)
project(varint C)

# Enable testing - allows "make test" or "ctest" to run all tests
enable_testing()

add_subdirectory(src)
add_subdirectory(examples)

# Custom target to run comprehensive tests with sanitizers
add_custom_target(test-comprehensive
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/test/test_all_comprehensive.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running comprehensive test suite with sanitizers"
    DEPENDS
        # Unit tests
        varintPackedTest varintDimensionTest
        varintDeltaTest varintFORTest varintPFORTest
        varintGroupTest varintDictTest varintAdaptiveTest
        varintFloatTest varintBitmapTest
        # Standalone examples (only those defined in CMake)
        example_tagged example_external example_split example_chained
        example_packed example_bitstream example_dimension
)

# Custom target to check for compiler warnings across all compilers
add_custom_target(check-warnings
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build/run_all_compilers.sh
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Checking for compiler warnings with GCC and Clang"
)

# Custom target for pre-upload validation: runs cppcheck + all tests
add_custom_target(check
    COMMAND cppcheck --enable=warning,style,performance,portability
            --error-exitcode=1 --inline-suppr --std=c11
            --check-level=exhaustive
            --suppress=missingIncludeSystem
            --suppress=unusedFunction
            # --suppress=unmatchedSuppression
            # --suppress=constVariablePointer
            # --suppress=constVariable
            # --suppress=constParameter
            # --suppress=constParameterPointer
            -I ${CMAKE_SOURCE_DIR}/src/ ${CMAKE_SOURCE_DIR}/src/*.c ${CMAKE_SOURCE_DIR}/examples/**/*.c
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running pre-upload validation: cppcheck + all tests"
    DEPENDS
        varintPackedTest varintDimensionTest
        varintDeltaTest varintFORTest varintPFORTest
        varintGroupTest varintDictTest varintAdaptiveTest
        varintFloatTest varintBitmapTest
)

# vi:ai et sw=4 ts=4:
