cmake_minimum_required(VERSION 3.10)
project(varint
    VERSION 1.0.0
    DESCRIPTION "High-performance varint encoding library for C"
    LANGUAGES C
)

# ============================================================================
# Options for when used as a dependency via FetchContent/add_subdirectory
# ============================================================================
option(VARINT_BUILD_TESTS "Build varint tests" ON)
option(VARINT_BUILD_EXAMPLES "Build varint examples" ON)

# Detect if we're the top-level project or included as a dependency
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(VARINT_IS_TOP_LEVEL ON)
else()
    set(VARINT_IS_TOP_LEVEL OFF)
    # When included as a dependency, default to not building tests/examples
    if(NOT DEFINED VARINT_BUILD_TESTS)
        set(VARINT_BUILD_TESTS OFF)
    endif()
    if(NOT DEFINED VARINT_BUILD_EXAMPLES)
        set(VARINT_BUILD_EXAMPLES OFF)
    endif()
endif()

# Enable testing only if building tests
if(VARINT_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src)

if(VARINT_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ============================================================================
# Development targets (only when building as top-level project)
# ============================================================================
if(VARINT_IS_TOP_LEVEL AND VARINT_BUILD_TESTS)
    # Custom target to run comprehensive tests with sanitizers
    add_custom_target(varint-test-comprehensive
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
    add_custom_target(varint-check-warnings
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/build/run_all_compilers.sh
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Checking for compiler warnings with GCC and Clang"
    )

    # Custom target for pre-upload validation: runs cppcheck + all tests
    add_custom_target(varint-check
        COMMAND cppcheck --enable=warning,style,performance,portability
                --error-exitcode=1 --inline-suppr --std=c11
                --check-level=exhaustive
                --suppress=missingIncludeSystem
                --suppress=unusedFunction
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
endif()

# ============================================================================
# Install & Export configuration (for find_package support)
# ============================================================================
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Install library targets
install(TARGETS varint-static varint-library varintDimension-static varintDimension-library
    EXPORT varintTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install headers
install(DIRECTORY src/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/varint
    FILES_MATCHING PATTERN "*.h"
)

# Generate and install export file
install(EXPORT varintTargets
    FILE varintTargets.cmake
    NAMESPACE varint::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/varint
)

# Generate version file
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/varintConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

# Install config files
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/varintConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/varintConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/varint
)

# vi:ai et sw=4 ts=4:
