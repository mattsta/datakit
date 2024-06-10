cmake_minimum_required(VERSION 3.5)

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

include(CheckCSourceRuns)

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
# Reset check flags
set(CMAKE_REQUIRED_FLAGS)

if(DK_HAS_ISNAN)
	add_definitions(-DDK_HAS_ISNAN)
endif()

if(DK_HAS_ISFINITE)
	add_definitions(-DDK_HAS_ISFINITE)
endif()

if(DK_HAS_ISINF)
	add_definitions(-DDK_HAS_ISINF)
endif()

if(DK_HAS_FLOAT16)
	add_definitions(-DDK_HAS_FL16)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mf16c")
endif()

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

if (DK_HAVE_GETRANDOM)
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

# If you need debug, build with:
# cmake -DCMAKE_BUILD_TYPE=Debug ..
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Winline -Wextra -std=gnu99 -Wno-missing-field-initializers")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wstrict-overflow=5 -Wfloat-equal")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wsuggest-attribute=const -Wsuggest-attribute=pure -Wsuggest-attribute=noreturn -Wsuggest-attribute=format")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wshadow -fstrict-aliasing -Wunused-macros")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wformat -Wignored-attributes")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-math-errno") # We don't use errno for libc math
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wempty-init-stmt -Wextra-semi-stmt")

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
# We know we're using computed gotos on clang and it's really okay
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-gnu-label-as-value")
endif()

#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -fno-omit-frame-pointer -fno-optimize-sibling-calls")

#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=undefined")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=memory")

# We know we abuse system alignment in a few places. Don't warn us.
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-sanitize=alignment")

#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=dataflow")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=cfi")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=safe-stack")

if(CMAKE_BUILD_TYPE MATCHES Debug)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -Wno-format")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3")
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

    multiarray.c
    multiarraySmall.c
    multiarrayMedium.c
    multiarrayLarge.c

    multiroar.c

    multilru.c

    multidict.c

    multiTimer.c
    timeUtil.c

    mds.c
    mdsc.c
    dj.c

    dod.c
    xof.c

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
    # Fix, or make conditional to only include if X64
    # intersectInt.c
    # intsetBig.c
    intset.c
    intsetU32.c
    hyperloglog.c
    databox.c
    databoxLinear.c
    datakit.c)

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

add_library(${PROJECT_NAME}-shared  MODULE $<TARGET_OBJECTS:${PROJECT_NAME}> ${deps})
add_library(${PROJECT_NAME}-static  STATIC $<TARGET_OBJECTS:${PROJECT_NAME}> ${deps})
add_library(${PROJECT_NAME}-library SHARED $<TARGET_OBJECTS:${PROJECT_NAME}> ${deps})

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
    # str is C11 instead of C99 for testing
    set_source_files_properties(str.c PROPERTIES COMPILE_FLAGS "-std=c11 -mcpu=native")
    set_source_files_properties(bigmath-libbf.c PROPERTIES COMPILE_FLAGS "-std=c11 -mcpu=native")
    add_definitions(-DDATAKIT_TEST=1)

    add_executable(${PROJECT_NAME}-test datakit-test.c)
    target_link_libraries(${PROJECT_NAME}-test ${PROJECT_NAME}-static)

    if(APPLE)
        add_custom_command(TARGET ${PROJECT_NAME}-test POST_BUILD COMMAND dsymutil ${PROJECT_NAME}-test COMMENT "Generating OS X Debug Info")
    endif()

    enable_testing()

    set(datakitTests
        flex
        multilistFull
        util
        mds
        mdsc
        hyperloglog
        offsetArray
        multimapFull
        multimap
        multiarrayLarge
        multiarraySmall
        multiarrayMedium
        multiarray
        databox
        str
        strDoubleFormat
        multiroar
        dod
        xof
        fibbuf
        jebuf
        mflex
        membound
        multiPriorityQueue
        )

    add_custom_target(check
        COMMAND ${CMAKE_CTEST_COMMAND} -VV
        DEPENDS ${PROJECT_NAME}-test)

    foreach(test ${datakitTests})
        add_test(NAME ${test} COMMAND ${PROJECT_NAME}-test test ${test})
    endforeach()

endif()

if(Build32Bit)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m32")
    set_target_properties(${PROJECT_NAME}-shared
        PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(${PROJECT_NAME}-static
        PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
    set_target_properties(${PROJECT_NAME}-library
        PROPERTIES COMPILE_FLAGS "-m32" LINK_FLAGS "-m32")
endif()
# vi:ai et sw=4 ts=4:
