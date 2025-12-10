cmake_minimum_required(VERSION 3.10)

set(DK_VERSION 0.3.0)
set(DK_ABI_UPDATE 0.3.0)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(CMAKE_SHARED_MODULE_CREATE_C_FLAGS
        "${CMAKE_SHARED_MODULE_CREATE_C_FLAGS} -undefined dynamic_lookup")
    cmake_policy(SET CMP0042 NEW)
elseif(${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    # Nothing special
else()
    # Untested platform?
endif()

option(Build32Bit "Build 32-bit Library" OFF)
option(UseC23 "Use C23 standard (enables _Float128 if available)" OFF)
option(UseFloat128 "Force enable __float128 support (requires libquadmath on some platforms)" OFF)
option(ForceNoFloat128 "Force disable all float128 support (for testing fallback paths)" OFF)

# Sanitizer options - enable with cmake -DEnableASAN=ON, etc.
option(EnableASAN "Enable AddressSanitizer" OFF)
option(EnableUBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(EnableMSAN "Enable MemorySanitizer (Clang only)" OFF)
option(EnableTSAN "Enable ThreadSanitizer" OFF)

include(CheckCSourceRuns)
include(CheckCSourceCompiles)

# ============================================================================
# Feature Detection: Math Functions
# ============================================================================
check_c_source_runs("
#include <math.h>
int main() {
    isnan(300.0);
    return 0;
}"
DK_HAS_ISNAN)

check_c_source_runs("
#include <math.h>
int main() {
    isfinite(300.0);
    return 0;
}"
DK_HAS_ISFINITE)

check_c_source_runs("
#include <math.h>
int main() {
    isinf(300.0);
    return 0;
}"
DK_HAS_ISINF)

# ============================================================================
# Feature Detection: x86 SIMD Extensions
# ============================================================================

# Float16 (F16C) is x86-specific
if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|i[3-6]86)")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_C_FLAGS} -mf16c")
    check_c_source_runs("
    #include <emmintrin.h>
    #include <assert.h>

    #define convertFloatToHalf(v) _cvtss_sh(v, 0)
    #define convertHalfToFloat(v) _cvtsh_ss(v)
    int main() {
        const float convertThis = 1.25;
        assert(convertHalfToFloat(convertFloatToHalf(convertThis)) == convertThis);
        return 0;
    }"
    DK_HAS_FLOAT16)
    set(CMAKE_REQUIRED_FLAGS)
endif()

# Check for AVX-512 VPOPCNT support (Ice Lake+ CPUs, ~2019)
# This requires both AVX-512F (foundation) and AVX-512 VPOPCNTDQ extension
if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64)")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_C_FLAGS} -mavx512f -mavx512vpopcntdq")
    check_c_source_compiles("
    #include <immintrin.h>
    int main() {
        __m512i v = _mm512_setzero_si512();
        __m512i p = _mm512_popcnt_epi64(v);
        long long sum = _mm512_reduce_add_epi64(p);
        return (int)sum;
    }"
    DK_HAS_AVX512_VPOPCNT)
    set(CMAKE_REQUIRED_FLAGS)

    if(DK_HAS_AVX512_VPOPCNT)
        message(STATUS "AVX-512 VPOPCNT: Supported - enabling optimized popcount")
    else()
        message(STATUS "AVX-512 VPOPCNT: Not supported (requires Ice Lake+ CPU or cross-compile flags)")
    endif()
endif()

# ============================================================================
# Apply Detected Features: Math Functions
# ============================================================================
if(DK_HAS_ISNAN)
    add_definitions(-DDK_HAS_ISNAN)
endif()

if(DK_HAS_ISFINITE)
    add_definitions(-DDK_HAS_ISFINITE)
endif()

if(DK_HAS_ISINF)
    add_definitions(-DDK_HAS_ISINF)
endif()

# ============================================================================
# Apply Detected Features: x86 SIMD
# ============================================================================
if(DK_HAS_FLOAT16)
    add_definitions(-DDK_HAS_FL16)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mf16c")
    message(STATUS "Float16 (F16C): Enabled")
endif()

# AVX-512 VPOPCNT: Apply flags globally since str.c (which includes strPopcnt.c)
# is compiled as part of the main library. The code has fallback paths for when
# these intrinsics aren't available.
if(DK_HAS_AVX512_VPOPCNT)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mavx512f -mavx512vpopcntdq")
endif()

# ============================================================================
# Feature Detection: Random Number Sources
# ============================================================================
check_c_source_runs("
#include <unistd.h>
#include <sys/random.h>
int main(void) {
    char  buf[4];

    if (getrandom(buf, 4, 0) < 0) {
        return 1;
    }

    return 0;
}" DK_HAVE_GETRANDOM)

check_c_source_runs("
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/random.h>
int main(void) {
    char  buf[4];

    if (syscall(SYS_getrandom, buf, 4, 0) < 0) {
        return 1;
    }

    return 0;
}" DK_HAVE_LINUX_SYS_GETRANDOM)

check_c_source_runs("
#include <unistd.h>
int main(void) {
    char  buf[4];

    if (getentropy(buf, 4) == -1) {
        return 1;
    }

    return 0;
}" DK_HAVE_GETENTROPY)

check_c_source_runs("
#include <unistd.h>
#include <sys/random.h>
int main(void) {
    char  buf[4];

    if (getentropy(buf, 4) == -1) {
        return 1;
    }

    return 0;
}" DK_HAVE_GETENTROPY_SYS_RANDOM)

# ============================================================================
# Apply Detected Features: Random Number Sources
# ============================================================================
if(DK_HAVE_GETRANDOM)
    add_definitions(-DDK_HAVE_GETRANDOM)
endif()

if(DK_HAVE_LINUX_SYS_GETRANDOM)
    add_definitions(-DDK_HAVE_LINUX_SYS_GETRANDOM)
endif()

if(DK_HAVE_GETENTROPY)
    add_definitions(-DDK_HAVE_GETENTROPY)
endif()

if(DK_HAVE_GETENTROPY_SYS_RANDOM)
    add_definitions(-DDK_HAVE_GETENTROPY_SYS_RANDOM)
endif()

# ============================================================================
# Extended Precision Float Detection (float128 / long double)
# ============================================================================
# Priority:
#   1. ForceNoFloat128 - disables all extended precision (for testing)
#   2. UseFloat128 - force enable __float128
#   3. UseC23 - enable C23 with _Float128
#   4. Auto-detect __float128 support
#   5. Check if long double has extended precision (x86)

if(ForceNoFloat128)
    message(STATUS "Float128: Disabled (ForceNoFloat128=ON)")
    add_definitions(-DDK_FORCE_NO_FLOAT128=1)
else()
    # Check for __float128 support
    check_c_source_compiles("
    int main() {
        __float128 x = 1.0Q;
        __float128 y = x * 2.0Q;
        return (int)y;
    }" DK_HAS_FLOAT128_NATIVE)

    # Check for _Float128 support (C23)
    set(CMAKE_REQUIRED_FLAGS "-std=c23")
    check_c_source_compiles("
    int main() {
        _Float128 x = 1.0F128;
        _Float128 y = x * 2.0F128;
        return (int)y;
    }" DK_HAS_FLOAT128_C23)
    set(CMAKE_REQUIRED_FLAGS)

    # Check long double precision
    check_c_source_runs("
    #include <float.h>
    int main() {
        /* Return 0 (success) if long double has >= 64 bit mantissa */
        return (LDBL_MANT_DIG >= 64) ? 0 : 1;
    }" DK_LONG_DOUBLE_EXTENDED)

    if(UseFloat128 AND DK_HAS_FLOAT128_NATIVE)
        message(STATUS "Float128: Enabled via __float128 (UseFloat128=ON)")
        add_definitions(-DDK_HAS_FLOAT128=1)
        # On some platforms (e.g., ARM Linux with GCC), libquadmath is needed
        if(NOT APPLE)
            find_library(QUADMATH_LIBRARY quadmath)
            if(QUADMATH_LIBRARY)
                message(STATUS "Float128: Found libquadmath: ${QUADMATH_LIBRARY}")
            endif()
        endif()
    elseif(UseC23 AND DK_HAS_FLOAT128_C23)
        message(STATUS "Float128: Enabled via C23 _Float128 (UseC23=ON)")
        add_definitions(-DDK_HAS_FLOAT128=1)
    elseif(DK_HAS_FLOAT128_NATIVE AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "arm|aarch64")
        # Auto-enable on x86 where it's well-supported
        message(STATUS "Float128: Auto-enabled via __float128 (x86)")
        add_definitions(-DDK_HAS_FLOAT128=1)
    elseif(DK_LONG_DOUBLE_EXTENDED)
        message(STATUS "Float128: Using native long double (LDBL_MANT_DIG >= 64)")
    else()
        message(STATUS "Float128: Not available - using integer fallback for precision-sensitive comparisons")
    endif()
endif()

# ============================================================================
# Compiler Configuration
# ============================================================================
add_definitions(-D_GNU_SOURCE)  # For memmem and other GNU extensions

# Set C standard based on UseC23 option
if(UseC23)
    set(DK_C_STANDARD "-std=gnu23")
    message(STATUS "C Standard: C23 (gnu23)")
else()
    set(DK_C_STANDARD "-std=gnu11")
    message(STATUS "C Standard: C11 (gnu11)")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Winline -Wextra ${DK_C_STANDARD} -Wno-missing-field-initializers")
# SSSE3 intrinsics like _mm_shuffle_epi8 are only available on x86_64
if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|i[3-6]86)")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mssse3")
endif()
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wstrict-overflow=5 -Wfloat-equal")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wsuggest-attribute=const -Wsuggest-attribute=pure -Wsuggest-attribute=noreturn -Wsuggest-attribute=format")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wshadow -fstrict-aliasing -Wunused-macros")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wformat -Wignored-attributes")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-math-errno") # We don't use errno for libc math

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    # Clang-specific warnings
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wempty-init-stmt -Wextra-semi-stmt")
# We know we're using computed gotos on clang and it's really okay
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-gnu-label-as-value")
endif()

# ============================================================================
# Sanitizer Configuration
# ============================================================================
# Enable with: cmake -DEnableASAN=ON ..
# Note: ASAN and TSAN are mutually exclusive

if(EnableASAN)
    message(STATUS "Sanitizer: AddressSanitizer enabled")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer -fno-optimize-sibling-calls")
endif()

if(EnableUBSAN)
    message(STATUS "Sanitizer: UndefinedBehaviorSanitizer enabled")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=undefined")
    # We know we abuse system alignment in a few places. Don't warn us.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-sanitize=alignment")
endif()

if(EnableMSAN)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        message(STATUS "Sanitizer: MemorySanitizer enabled")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=memory")
    else()
        message(WARNING "MemorySanitizer requires Clang - ignoring EnableMSAN")
    endif()
endif()

if(EnableTSAN)
    message(STATUS "Sanitizer: ThreadSanitizer enabled")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=thread")
endif()

# Even without sanitizers, disable alignment sanitizer (we intentionally use unaligned access)
if(NOT EnableUBSAN)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-sanitize=alignment")
endif()

if(CMAKE_BUILD_TYPE MATCHES Debug)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g -Wno-format")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g")
endif()

# DATAKIT_TEST mode enables debug/repr functions like databoxReprStr, flexRepr, etc.
# This can be enabled independently of BuildTestBinary so consuming projects can
# use debug symbols without building the datakit test executable.
option(DatakitTestMode "Enable DATAKIT_TEST mode for debug symbols (independent of BuildTestBinary)" On)

if(DatakitTestMode)
    add_definitions(-DDATAKIT_TEST=1)
    # Tests require assertions - ensure NDEBUG is not defined
    add_definitions(-UNDEBUG)
endif()

add_library(${PROJECT_NAME} OBJECT
    fibbuf.c
    jebuf.c

    float16.c

    dataspeed.c

    multilist.c
    multilistSmall.c
    multilistMedium.c
    multilistFull.c

    multimap.c
    multimapSmall.c
    multimapMedium.c
    multimapFull.c

    multimapAtom.c

    stringPool.c
    atomPool.c

    multiarray.c
    multiarraySmall.c
    multiarrayMedium.c
    multiarrayLarge.c

    multiroar.c

    multiOrderedSet.c
    multiOrderedSetSmall.c
    multiOrderedSetMedium.c
    multiOrderedSetFull.c

    multilru.c

    multidict.c

    multiTimer.c
    timerWheel.c
    timerWheelTest.c
    timeUtil.c

    mds.c
    mdsc.c
    dj.c

    dod.c
    xof.c
    compressionBench.c
    bbits.c
    linearBloomTest.c
    multiTimerTest.c

    flex.c
    mflex.c

    OSRegulate.c
    setproctitle.c
    versionOSRuntime.c

    membound.c
    ptrPrevNext.c

    memtest.c

    bigmath-imath.c
    # bigmath-libbf.c
    str.c
    strDoubleFormat.c
    util.c
    portableRandom.c
    # quickcount.c

    # quickdictClassAtom.c
    # quickdictClassAtomicMap.c

    # Only for testing
    offsetArray.c

    # patternTrie.c
    # Temporary (?) reductions due to not compiling on ARM.
    intersectInt.c
    intsetBig.c
    intset.c
    intsetSmall.c
    intsetMedium.c
    intsetFull.c
    intsetTest.c
    intsetU32.c

    # Fenwick & Segment tree implementations (2-tier template system)
    # Note: Old fenwick.c, segment.c files moved to old/ directory
    # Using new type-specific implementations added via add_subdirectory()

    # multiFenwick wraps multiple Fenwick trees - separate system
    multiFenwick.c
    multiFenwickTest.c

    # multiSegment wraps multiple Segment trees - separate system
    # DISABLED: multiSegment.c
    # DISABLED: multiSegmentTest.c

    hyperloglog.c
    databox.c
    databoxLinear.c
    floatExtendedTest.c
    datakit.c
    ulid.c
    persist.c

    # Persistent data structure wrappers
    persist/persistCtx.c
    persist/multimapP.c
    persist/multilistP.c
    persist/multidictP.c
    persist/flexP.c
    persist/intsetP.c
    persist/multiOrderedSetP.c
    persist/multilruP.c
    persist/multiroarP.c
)

# ============================================================================
# Fenwick Tree Template System (2-Tier Architecture)
# ============================================================================
# Add all type-specific Fenwick tree implementations
# Architecture: Small (cache-friendly) → Full (unlimited)
# 10 types × 4 files = 40 files generated from 3 templates
add_subdirectory(fenwick)

# ============================================================================
# Segment Tree Template System (2-Tier Architecture)
# ============================================================================
# Add all type-specific Segment tree implementations
# Architecture: Small (eager) → Full (lazy propagation)
# 10 types × 4 files = 40 files generated from 3 templates
add_subdirectory(segment)

# No optimizations on speed test so results don't get evaluated at compile time
set_source_files_properties(dataspeed.c
                            PROPERTIES
                            COMPILE_FLAGS "-O0")

set(deps $<TARGET_OBJECTS:lz4>
         $<TARGET_OBJECTS:sha1>
         #         $<TARGET_OBJECTS:libbf>
         $<TARGET_OBJECTS:varint>

         # xxhash is needed for lz4 compression.
         # we also use xxhash in hyperloglog, but there we
         # include it all static inline.
         $<TARGET_OBJECTS:xxhash>)

# Additional varint modules for compression benchmarking
# These are not in the core varint library but are standalone implementations
set(VARINT_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../deps/varint/src)
set(VARINT_EXTRA_SOURCES
    ${VARINT_SRC_DIR}/varintDelta.c
    ${VARINT_SRC_DIR}/varintFloat.c
    ${VARINT_SRC_DIR}/varintBP128.c)

add_library(${PROJECT_NAME}-shared  MODULE $<TARGET_OBJECTS:${PROJECT_NAME}> ${deps} ${VARINT_EXTRA_SOURCES})
add_library(${PROJECT_NAME}-static  STATIC $<TARGET_OBJECTS:${PROJECT_NAME}> ${deps} ${VARINT_EXTRA_SOURCES})
add_library(${PROJECT_NAME}-library SHARED $<TARGET_OBJECTS:${PROJECT_NAME}> ${deps} ${VARINT_EXTRA_SOURCES})

# Don't prefix "lib"
set(CMAKE_STATIC_LIBRARY_PREFIX "")
set(CMAKE_SHARED_MODULE_PREFIX "")
#set(CMAKE_SHARED_LIBRARY_PREFIX "")

set_target_properties(${PROJECT_NAME}-shared  PROPERTIES OUTPUT_NAME ${PROJECT_NAME})
set_target_properties(${PROJECT_NAME}-static  PROPERTIES OUTPUT_NAME ${PROJECT_NAME})
set_target_properties(${PROJECT_NAME}-library PROPERTIES OUTPUT_NAME ${PROJECT_NAME})

if(NOT APPLE)
    target_link_libraries(${PROJECT_NAME}-static pthread)
    target_link_libraries(${PROJECT_NAME}-library pthread)
    target_link_libraries(${PROJECT_NAME}-static m)
    target_link_libraries(${PROJECT_NAME}-library m)
endif()

# SOVERSION only needs to increment when introducing *breaking* changes.
# Otherwise, just increase VERSION with normal feature additions or maint.
set_target_properties(${PROJECT_NAME}-library PROPERTIES VERSION ${DK_VERSION} SOVERSION ${DK_ABI_UPDATE})

option(BuildTestBinary "Build test binary" On)

if(BuildTestBinary)
    # Enable native CPU optimizations for performance-critical code
    # Note: str.c requires -std=c11 to ensure consistent floating-point behavior
    # Use -march=native on x86, -mcpu=native on ARM
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|i[3-6]86)")
        set(DK_NATIVE_FLAG "-march=native")
    else()
        set(DK_NATIVE_FLAG "-mcpu=native")
    endif()
    set_source_files_properties(str.c PROPERTIES COMPILE_FLAGS "-std=c11 ${DK_NATIVE_FLAG}")
    set_source_files_properties(bigmath-libbf.c PROPERTIES COMPILE_FLAGS "-std=c11 ${DK_NATIVE_FLAG}")

    add_executable(${PROJECT_NAME}-test datakit-test.c
                   fenwick/fenwickI64Test.c
                   segment/segmentI64Test.c)
    target_link_libraries(${PROJECT_NAME}-test ${PROJECT_NAME}-static)

    if(APPLE)
        add_custom_command(TARGET ${PROJECT_NAME}-test POST_BUILD COMMAND dsymutil ${PROJECT_NAME}-test COMMENT "Generating OS X Debug Info")
    endif()

    # Note: enable_testing() is called in top-level CMakeLists.txt

    # ========================================================================
    # Auto-extract test names from datakit-test.c (single source of truth)
    # ========================================================================
    # Parse the testRegistry in datakit-test.c to extract test names.
    # This eliminates duplicate maintenance - just add tests to the C file.

    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/datakit-test.c" DATAKIT_TEST_SOURCE)

    # Extract the testRegistry block (between "testRegistry[] = {" and "T_END")
    string(REGEX MATCH "testRegistry\\[\\] = \\{[^}]+T_END" TEST_REGISTRY_BLOCK "${DATAKIT_TEST_SOURCE}")

    # Extract test names from T(name), T_A(name, ...), T_ADJ(name), T_A_ADJ(name, ...)
    # Pattern matches: T(foo) or T_A(foo, "...") or T_ADJ(foo) or T_A_ADJ(foo, "...")
    string(REGEX MATCHALL "T(_A)?(_ADJ)?\\([a-zA-Z0-9_]+" TEST_ENTRIES "${TEST_REGISTRY_BLOCK}")

    set(datakitTests "")
    foreach(entry ${TEST_ENTRIES})
        # Extract just the test name (after the opening paren)
        string(REGEX REPLACE "T(_A)?(_ADJ)?\\(" "" testName "${entry}")
        list(APPEND datakitTests ${testName})
    endforeach()

    # Handle conditional tests: intsetBig requires 128-bit integer support
    # The regex extracts ALL test names including intsetBig from inside #if blocks.
    # We need to remove it if the platform doesn't support int128.
    list(FIND datakitTests "intsetBig" INTSETBIG_INDEX)
    if(NOT INTSETBIG_INDEX EQUAL -1)
        check_c_source_compiles("
        int main() {
            __int128_t a = 1;
            __uint128_t b = 1;
            return (int)(a + b);
        }"
        DK_HAS_INT128)
        if(NOT DK_HAS_INT128)
            list(REMOVE_ITEM datakitTests "intsetBig")
            message(STATUS "Removed intsetBig test (no __int128 support)")
        endif()
    endif()

    # Report discovered tests
    list(LENGTH datakitTests NUM_TESTS)
    message(STATUS "Auto-discovered ${NUM_TESTS} tests from datakit-test.c")

    # Create test targets
    foreach(test ${datakitTests})
        add_test(NAME ${test} COMMAND ${PROJECT_NAME}-test test ${test})
    endforeach()

    # check target - runs all tests with verbose output
    add_custom_target(datakit-check
        COMMAND ${CMAKE_CTEST_COMMAND} -VV
        DEPENDS ${PROJECT_NAME}-test)

    # quickcheck target - runs tests without verbose output
    add_custom_target(datakit-quickcheck
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
        DEPENDS ${PROJECT_NAME}-test)

endif()

# ============================================================================
# Static Analysis with cppcheck
# ============================================================================
find_program(CPPCHECK_EXECUTABLE cppcheck)

if(CPPCHECK_EXECUTABLE)
    message(STATUS "Found cppcheck: ${CPPCHECK_EXECUTABLE}")

    # Get all source files
    file(GLOB_RECURSE ALL_SOURCE_FILES
        ${CMAKE_CURRENT_SOURCE_DIR}/*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

    # Exclude test file from analysis
    list(FILTER ALL_SOURCE_FILES EXCLUDE REGEX ".*datakit-test\\.c$")
    list(FILTER ALL_SOURCE_FILES EXCLUDE REGEX ".*ctest\\.h$")

    # cppcheck target - quick analysis
    add_custom_target(cppcheck
        COMMAND ${CPPCHECK_EXECUTABLE}
            --enable=warning,style,performance,portability
            --suppress=missingIncludeSystem
            --suppress=unusedFunction
            --suppress=unmatchedSuppression
            --inline-suppr
            --quiet
            --error-exitcode=0
            -I${CMAKE_CURRENT_SOURCE_DIR}
            -I${CMAKE_CURRENT_SOURCE_DIR}/../deps/lz4/lib
            -I${CMAKE_CURRENT_SOURCE_DIR}/../deps/varint/src
            -I${CMAKE_CURRENT_SOURCE_DIR}/../deps/imath/src
            -I${CMAKE_CURRENT_SOURCE_DIR}/../deps/xxHash
            ${ALL_SOURCE_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running cppcheck static analysis...")

    # cppcheck-full target - comprehensive analysis with all checks
    add_custom_target(cppcheck-full
        COMMAND ${CPPCHECK_EXECUTABLE}
            --enable=all
            --suppress=missingIncludeSystem
            --suppress=unusedFunction
            --suppress=unmatchedSuppression
            --inline-suppr
            --inconclusive
            --force
            --error-exitcode=0
            -I${CMAKE_CURRENT_SOURCE_DIR}
            -I${CMAKE_CURRENT_SOURCE_DIR}/../deps/lz4/lib
            -I${CMAKE_CURRENT_SOURCE_DIR}/../deps/varint/src
            -I${CMAKE_CURRENT_SOURCE_DIR}/../deps/imath/src
            -I${CMAKE_CURRENT_SOURCE_DIR}/../deps/xxHash
            ${ALL_SOURCE_FILES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running comprehensive cppcheck analysis...")

    # cppcheck-xml target - generate XML report for CI integration
    add_custom_target(cppcheck-xml
        COMMAND ${CPPCHECK_EXECUTABLE}
            --enable=all
            --suppress=missingIncludeSystem
            --suppress=unusedFunction
            --suppress=unmatchedSuppression
            --inline-suppr
            --xml
            --xml-version=2
            -I${CMAKE_CURRENT_SOURCE_DIR}
            -I${CMAKE_CURRENT_SOURCE_DIR}/../deps/lz4/lib
            -I${CMAKE_CURRENT_SOURCE_DIR}/../deps/varint/src
            -I${CMAKE_CURRENT_SOURCE_DIR}/../deps/imath/src
            -I${CMAKE_CURRENT_SOURCE_DIR}/../deps/xxHash
            ${ALL_SOURCE_FILES}
            2> ${CMAKE_BINARY_DIR}/cppcheck-report.xml
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating cppcheck XML report...")

else()
    message(STATUS "cppcheck not found - static analysis targets disabled")
endif()

# ============================================================================
# Lint target - combines multiple checks
# ============================================================================
add_custom_target(lint
    COMMENT "Running all lint checks...")

if(CPPCHECK_EXECUTABLE)
    add_dependencies(lint cppcheck)
endif()

# vi:ai et sw=4 ts=4:
