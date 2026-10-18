# List every test of a configured build tree with its command line.
#
#   cmake -DDIR=<binary dir> -DOUT=<file> -P ctestCommands.cmake
#
# Evaluates DIR's CTestTestfile.cmake files the way ctest reads them,
# with add_test and subdirs redefined to record instead of run, so
# CMake's own parser handles quoting and per-configuration blocks.
# Unlike `ctest --show-only=json-v1`, this names a test's executable
# even before that executable is built.
#
# OUT gets one record per test, each ended by ASCII 30 (record
# separator): the test name, then each command argument, separated
# by ASCII 31 (unit separator).

cmake_minimum_required(VERSION 3.20)

if(NOT DIR OR NOT OUT)
    message(FATAL_ERROR "usage: cmake -DDIR=<binary dir> -DOUT=<file> -P ${CMAKE_CURRENT_LIST_FILE}")
endif()

string(ASCII 30 _dktoolsRecord)
string(ASCII 31 _dktoolsUnit)
file(WRITE "${OUT}" "")

function(add_test name)
    set(record "${name}")
    if(ARGC GREATER 1)
        math(EXPR last "${ARGC} - 1")
        foreach(i RANGE 1 ${last})
            string(APPEND record "${_dktoolsUnit}${ARGV${i}}")
        endforeach()
    endif()
    file(APPEND "${OUT}" "${record}${_dktoolsRecord}")
endfunction()

function(subdirs)
    foreach(dir IN LISTS ARGV)
        if(NOT IS_ABSOLUTE "${dir}")
            set(dir "${CMAKE_CURRENT_LIST_DIR}/${dir}")
        endif()
        include("${dir}/CTestTestfile.cmake" OPTIONAL)
    endforeach()
endfunction()

function(set_tests_properties)
endfunction()

function(set_directory_properties)
endfunction()

include("${DIR}/CTestTestfile.cmake")
