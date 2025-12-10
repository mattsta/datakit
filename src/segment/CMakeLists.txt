# Segment Tree Template System - 2-Tier Architecture

set(SEGMENT_TYPES I16 I32 I64 U16 U32 U64 Float Double)
set(SEGMENT_128BIT_TYPES I128 U128)

set(SEGMENT_CORE_HEADERS
    segmentCore.h
    segmentCoreImpl.h
    segmentCoreFullImpl.h
    ${CMAKE_SOURCE_DIR}/src/segmentCommon.h
)

set(SEGMENT_SOURCES "")
set(SEGMENT_HEADERS "")

foreach(TYPE ${SEGMENT_TYPES})
    list(APPEND SEGMENT_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/segment${TYPE}.c
        ${CMAKE_CURRENT_SOURCE_DIR}/segment${TYPE}Small.c
        ${CMAKE_CURRENT_SOURCE_DIR}/segment${TYPE}Full.c
    )
    list(APPEND SEGMENT_HEADERS
        ${CMAKE_CURRENT_SOURCE_DIR}/segment${TYPE}.h
    )
endforeach()

# Conditional 128-bit support
include(CheckTypeSize)
check_type_size("__int128_t" SIZEOF_INT128)

if(SIZEOF_INT128)
    message(STATUS "Segment: Enabling 128-bit integer support")
    foreach(TYPE ${SEGMENT_128BIT_TYPES})
        list(APPEND SEGMENT_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/segment${TYPE}.c
            ${CMAKE_CURRENT_SOURCE_DIR}/segment${TYPE}Small.c
            ${CMAKE_CURRENT_SOURCE_DIR}/segment${TYPE}Full.c
        )
        list(APPEND SEGMENT_HEADERS
            ${CMAKE_CURRENT_SOURCE_DIR}/segment${TYPE}.h
        )
    endforeach()
else()
    message(STATUS "Segment: 128-bit integers not supported")
endif()

# Add to parent target
target_sources(datakit PRIVATE ${SEGMENT_SOURCES})

# Print summary
list(LENGTH SEGMENT_SOURCES SEGMENT_SOURCE_COUNT)
list(LENGTH SEGMENT_HEADERS SEGMENT_HEADER_COUNT)
message(STATUS "Segment: Generated ${SEGMENT_SOURCE_COUNT} source files from 3 templates")
message(STATUS "Segment: ${SEGMENT_HEADER_COUNT} type-specific headers")
message(STATUS "Segment: 2-tier architecture (Small → Full)")
