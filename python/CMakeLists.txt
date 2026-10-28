cmake_minimum_required(VERSION 4.4)
project(datakit_python C)

# The extension targets the stable ABI shared by the regular and the
# free-threaded interpreters (abi3t), 3.15 and later
find_package(Python 3.15 REQUIRED COMPONENTS Interpreter Development.SABIModule)

# datakit itself, built here as the library the extension links
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. datakit EXCLUDE_FROM_ALL)

Python_add_library(_datakit MODULE USE_SABI 3.15 WITH_SOABI
    src/_datakit/module.c
    src/_datakit/persist.c
    src/_datakit/testing.c
)
target_compile_definitions(_datakit PRIVATE Py_TARGET_ABI3T=0x030f0000)
target_compile_options(_datakit PRIVATE -std=gnu23 -Wall -Wextra
    -Wno-missing-field-initializers -fno-omit-frame-pointer)
target_include_directories(_datakit PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(_datakit PRIVATE datakit-static)

install(TARGETS _datakit LIBRARY DESTINATION datakit)
