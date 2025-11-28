set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(CMAKE_SHARED_MODULE_CREATE_C_FLAGS
        "${CMAKE_SHARED_MODULE_CREATE_C_FLAGS} -undefined dynamic_lookup")
    cmake_policy(SET CMP0042 NEW)
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pedantic -std=c11")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wshadow -mtune=native")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wstrict-aliasing -Wstrict-overflow")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-missing-field-initializers")

# If you need debug, build with:
# cmake -DCMAKE_BUILD_TYPE=Debug ..
if(CMAKE_BUILD_TYPE MATCHES Debug)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3")
endif()

add_library(${PROJECT_NAME} OBJECT
    varintExternal.c
    varintExternalBigEndian.c
    varintChained.c
    varintChainedSimple.c
    varintTagged.c)

set(DIMENSION ${PROJECT_NAME}Dimension)
set(PACKED ${PROJECT_NAME}Packed)

add_library(${DIMENSION} OBJECT
    varintExternal.c
    varintTagged.c
    varintDimension.c)

add_library(${PROJECT_NAME}-shared  MODULE $<TARGET_OBJECTS:${PROJECT_NAME}>)
add_library(${PROJECT_NAME}-static  STATIC $<TARGET_OBJECTS:${PROJECT_NAME}>)
add_library(${PROJECT_NAME}-library SHARED $<TARGET_OBJECTS:${PROJECT_NAME}>)

set_target_properties(${PROJECT_NAME}-shared  PROPERTIES OUTPUT_NAME ${PROJECT_NAME})
set_target_properties(${PROJECT_NAME}-static  PROPERTIES OUTPUT_NAME ${PROJECT_NAME})
set_target_properties(${PROJECT_NAME}-library PROPERTIES OUTPUT_NAME ${PROJECT_NAME})

add_library(${DIMENSION}-shared  MODULE $<TARGET_OBJECTS:${DIMENSION}>)
add_library(${DIMENSION}-static  STATIC $<TARGET_OBJECTS:${DIMENSION}>)
add_library(${DIMENSION}-library SHARED $<TARGET_OBJECTS:${DIMENSION}>)

set_target_properties(${DIMENSION}-shared  PROPERTIES OUTPUT_NAME ${DIMENSION})
set_target_properties(${DIMENSION}-static  PROPERTIES OUTPUT_NAME ${DIMENSION})
set_target_properties(${DIMENSION}-library PROPERTIES OUTPUT_NAME ${DIMENSION})

if(NOT APPLE)
    target_link_libraries(${DIMENSION}-static m)
    target_link_libraries(${DIMENSION}-library m)
endif()

# Don't prefix "lib"
set(CMAKE_STATIC_LIBRARY_PREFIX "")
set(CMAKE_SHARED_MODULE_PREFIX "")
#set(CMAKE_SHARED_LIBRARY_PREFIX "")

option(BuildTestBinary "Build test binary" On)
if(BuildTestBinary)
    add_executable(${PACKED}Test varintPackedTest.c)

    add_definitions(-DVARINT_DIMENSION_TEST)
    add_executable(${PROJECT_NAME}Compare varintCompare.c)
    target_link_libraries(${PROJECT_NAME}Compare ${PROJECT_NAME}-static)

    add_executable(${DIMENSION}Test varintDimensionTest.c)
    target_link_libraries(${DIMENSION}Test ${DIMENSION}-static)

    # ====================================================================
    # BUILD ALL UNIT TESTS
    # ====================================================================

    # varintDeltaTest - tests delta encoding with ZigZag
    add_executable(varintDeltaTest varintDeltaTest.c varintDelta.c varintExternal.c)
    target_compile_definitions(varintDeltaTest PRIVATE VARINT_DELTA_TEST_STANDALONE)

    # varintFORTest - tests Frame of Reference encoding
    add_executable(varintFORTest varintFORTest.c varintFOR.c varintExternal.c varintTagged.c)
    target_compile_definitions(varintFORTest PRIVATE VARINT_FOR_TEST_STANDALONE)

    # varintPFORTest - tests Patched Frame of Reference encoding
    add_executable(varintPFORTest varintPFORTest.c varintPFOR.c varintFOR.c varintExternal.c varintTagged.c)
    target_compile_definitions(varintPFORTest PRIVATE VARINT_PFOR_TEST_STANDALONE)

    # varintGroupTest - tests grouped varint encoding
    add_executable(varintGroupTest varintGroupTest.c varintGroup.c varintExternal.c)
    target_compile_definitions(varintGroupTest PRIVATE VARINT_GROUP_TEST_STANDALONE)

    # varintDictTest - tests dictionary encoding
    add_executable(varintDictTest varintDictTest.c varintDict.c varintExternal.c varintTagged.c)
    target_compile_definitions(varintDictTest PRIVATE VARINT_DICT_TEST_STANDALONE)

    # varintAdaptiveTest - tests adaptive encoding selection
    add_executable(varintAdaptiveTest varintAdaptiveTest.c
                   varintAdaptive.c varintDelta.c varintFOR.c varintPFOR.c
                   varintDict.c varintBitmap.c varintExternal.c varintTagged.c)
    target_compile_definitions(varintAdaptiveTest PRIVATE VARINT_ADAPTIVE_TEST_STANDALONE)

    # varintFloatTest - tests float compression
    add_executable(varintFloatTest varintFloatTest.c varintFloat.c varintExternal.c)
    target_compile_definitions(varintFloatTest PRIVATE VARINT_FLOAT_TEST_STANDALONE)

    # varintBitmapTest - tests bitmap encoding
    add_executable(varintBitmapTest varintBitmapTest.c varintBitmap.c varintExternal.c)

    # varintRLETest - tests run-length encoding
    add_executable(varintRLETest varintRLETest.c varintRLE.c varintExternal.c varintTagged.c)
    target_compile_definitions(varintRLETest PRIVATE VARINT_RLE_TEST)

    # varintEliasTest - tests Elias Gamma/Delta universal codes
    add_executable(varintEliasTest varintEliasTest.c varintElias.c)
    target_compile_definitions(varintEliasTest PRIVATE VARINT_ELIAS_TEST)
    target_compile_definitions(varintBitmapTest PRIVATE VARINT_BITMAP_TEST_STANDALONE)

    # varintBP128Test - tests SIMD block-packed encoding
    add_executable(varintBP128Test varintBP128Test.c varintBP128.c varintTagged.c)
    target_compile_definitions(varintBP128Test PRIVATE VARINT_BP128_TEST)

    # Link math library on non-Apple systems
    if(NOT APPLE)
        target_link_libraries(${PROJECT_NAME}Compare m)
        target_link_libraries(${DIMENSION}Test m)
        target_link_libraries(${PACKED}Test m)
        target_link_libraries(varintFloatTest m)
        target_link_libraries(varintAdaptiveTest m)
    endif()

    # ====================================================================
    # REGISTER ALL TESTS WITH CTEST
    # ====================================================================

    add_test(NAME varint-packed COMMAND ${PACKED}Test 1)
    add_test(NAME varint-dimension COMMAND ${DIMENSION}Test)
    add_test(NAME varint-delta COMMAND varintDeltaTest)
    add_test(NAME varint-for COMMAND varintFORTest)
    add_test(NAME varint-pfor COMMAND varintPFORTest)
    add_test(NAME varint-group COMMAND varintGroupTest)
    add_test(NAME varint-dict COMMAND varintDictTest)
    add_test(NAME varint-adaptive COMMAND varintAdaptiveTest)
    add_test(NAME varint-float COMMAND varintFloatTest)
    add_test(NAME varint-bitmap COMMAND varintBitmapTest)
    add_test(NAME varint-rle COMMAND varintRLETest)
    add_test(NAME varint-elias COMMAND varintEliasTest)
    add_test(NAME varint-bp128 COMMAND varintBP128Test)

    # Generate debug symbols on macOS
    if(APPLE)
        add_custom_command(TARGET ${PROJECT_NAME}Compare POST_BUILD COMMAND dsymutil ${PROJECT_NAME}Compare COMMENT "Generating OS X Debug Info")
        add_custom_command(TARGET ${DIMENSION}Test POST_BUILD COMMAND dsymutil ${DIMENSION}Test COMMENT "Generating OS X Debug Info")
        add_custom_command(TARGET ${PACKED}Test POST_BUILD COMMAND dsymutil ${PACKED}Test COMMENT "Generating OS X Debug Info")
        add_custom_command(TARGET varintDeltaTest POST_BUILD COMMAND dsymutil varintDeltaTest)
        add_custom_command(TARGET varintFORTest POST_BUILD COMMAND dsymutil varintFORTest)
        add_custom_command(TARGET varintPFORTest POST_BUILD COMMAND dsymutil varintPFORTest)
        add_custom_command(TARGET varintGroupTest POST_BUILD COMMAND dsymutil varintGroupTest)
        add_custom_command(TARGET varintDictTest POST_BUILD COMMAND dsymutil varintDictTest)
        add_custom_command(TARGET varintAdaptiveTest POST_BUILD COMMAND dsymutil varintAdaptiveTest)
        add_custom_command(TARGET varintFloatTest POST_BUILD COMMAND dsymutil varintFloatTest)
        add_custom_command(TARGET varintBitmapTest POST_BUILD COMMAND dsymutil varintBitmapTest)
        add_custom_command(TARGET varintRLETest POST_BUILD COMMAND dsymutil varintRLETest)
        add_custom_command(TARGET varintEliasTest POST_BUILD COMMAND dsymutil varintEliasTest)
        add_custom_command(TARGET varintBP128Test POST_BUILD COMMAND dsymutil varintBP128Test)
    endif()
endif()

# vi:ai et sw=4 ts=4:
