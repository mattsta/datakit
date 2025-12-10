# varintConfig.cmake - Package configuration for varint library
#
# This file is used by find_package(varint) to locate the library
# after installation. For FetchContent usage, this is not needed.
#
# Exported targets:
#   varint::varint-static      - Static library (core encodings)
#   varint::varint-library     - Shared library (core encodings)
#   varint::varintDimension-static  - Static library (dimension encodings)
#   varint::varintDimension-library - Shared library (dimension encodings)
#
# Example usage:
#   find_package(varint REQUIRED)
#   target_link_libraries(myapp PRIVATE varint::varint-static)

include(CMakeFindDependencyMacro)

# Include the exported targets
include("${CMAKE_CURRENT_LIST_DIR}/varintTargets.cmake")

# Provide convenient aliases without namespace for backwards compatibility
if(NOT TARGET varint-static AND TARGET varint::varint-static)
    add_library(varint-static ALIAS varint::varint-static)
endif()
if(NOT TARGET varint-library AND TARGET varint::varint-library)
    add_library(varint-library ALIAS varint::varint-library)
endif()
if(NOT TARGET varintDimension-static AND TARGET varint::varintDimension-static)
    add_library(varintDimension-static ALIAS varint::varintDimension-static)
endif()
if(NOT TARGET varintDimension-library AND TARGET varint::varintDimension-library)
    add_library(varintDimension-library ALIAS varint::varintDimension-library)
endif()
