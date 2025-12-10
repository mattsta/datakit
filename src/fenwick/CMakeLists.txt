# Fenwick Tree Template System - 2-Tier Architecture
# Auto-generated type-specific Fenwick tree implementations

# All supported types (2-tier: Small + Full)
set(FENWICK_TYPES I16 I32 I64 U16 U32 U64 Float Double)

# 128-bit types (conditional compilation)
set(FENWICK_128BIT_TYPES I128 U128)

# Core template headers (included by generated code)
set(FENWICK_CORE_HEADERS
    fenwickCore.h
    fenwickCoreImpl.h
    fenwickCoreFullImpl.h
    ${CMAKE_SOURCE_DIR}/src/fenwickCommon.h
)

# Generate source lists for all standard types
set(FENWICK_SOURCES "")
set(FENWICK_HEADERS "")

foreach(TYPE ${FENWICK_TYPES})
    list(APPEND FENWICK_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/fenwick${TYPE}.c          # Dispatcher
        ${CMAKE_CURRENT_SOURCE_DIR}/fenwick${TYPE}Small.c     # Small tier
        ${CMAKE_CURRENT_SOURCE_DIR}/fenwick${TYPE}Full.c      # Full tier
    )
    list(APPEND FENWICK_HEADERS
        ${CMAKE_CURRENT_SOURCE_DIR}/fenwick${TYPE}.h          # Public API
    )
endforeach()

# Conditional 128-bit support
include(CheckTypeSize)
check_type_size("__int128_t" SIZEOF_INT128)

if(SIZEOF_INT128)
    message(STATUS "Fenwick: Enabling 128-bit integer support")
    foreach(TYPE ${FENWICK_128BIT_TYPES})
        list(APPEND FENWICK_SOURCES
            ${CMAKE_CURRENT_SOURCE_DIR}/fenwick${TYPE}.c
            ${CMAKE_CURRENT_SOURCE_DIR}/fenwick${TYPE}Small.c
            ${CMAKE_CURRENT_SOURCE_DIR}/fenwick${TYPE}Full.c
        )
        list(APPEND FENWICK_HEADERS
            ${CMAKE_CURRENT_SOURCE_DIR}/fenwick${TYPE}.h
        )
    endforeach()
else()
    message(STATUS "Fenwick: 128-bit integers not supported on this platform")
endif()

# Add all fenwick sources to parent target (relative to parent)
target_sources(datakit PRIVATE ${FENWICK_SOURCES})

# Install headers if needed
if(DATAKIT_INSTALL_HEADERS)
    install(FILES ${FENWICK_HEADERS} ${FENWICK_CORE_HEADERS}
            DESTINATION include/datakit/fenwick)
endif()

# Testing - tests are added to datakit-test binary, not datakit library
# Test registration handled by datakit-test.c test registry

# Print summary
list(LENGTH FENWICK_SOURCES FENWICK_SOURCE_COUNT)
list(LENGTH FENWICK_HEADERS FENWICK_HEADER_COUNT)
message(STATUS "Fenwick: Generated ${FENWICK_SOURCE_COUNT} source files from 3 templates")
message(STATUS "Fenwick: ${FENWICK_HEADER_COUNT} type-specific headers")
message(STATUS "Fenwick: 2-tier architecture (Small → Full)")
