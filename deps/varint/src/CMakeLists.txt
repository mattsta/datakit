set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
    set(CMAKE_SHARED_MODULE_CREATE_C_FLAGS
        "${CMAKE_SHARED_MODULE_CREATE_C_FLAGS} -undefined dynamic_lookup")
    cmake_policy(SET CMP0042 NEW)
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -pedantic -std=c99")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wshadow -march=native")
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

# varintDimension.c uses F16C intrinsics on x86_64
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mf16c")

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

    if(APPLE)
        add_custom_command(TARGET ${PROJECT_NAME}Compare POST_BUILD COMMAND dsymutil ${PROJECT_NAME}Compare COMMENT "Generating OS X Debug Info")
        add_custom_command(TARGET ${DIMENSION}Test POST_BUILD COMMAND dsymutil ${DIMENSION}Test COMMENT "Generating OS X Debug Info")
        add_custom_command(TARGET ${PACKED}Test POST_BUILD COMMAND dsymutil ${PACKED}Test COMMENT "Generating OS X Debug Info")
    endif()

    if(NOT APPLE)
        target_link_libraries(${PROJECT_NAME}Compare m)
        target_link_libraries(${DIMENSION}Test m)
        target_link_libraries(${PACKED}Test m)
    endif()
endif()

# vi:ai et sw=4 ts=4:
