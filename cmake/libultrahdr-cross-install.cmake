# Run as: cmake -DSOURCE_DIR=<dir> -P libultrahdr-cross-install.cmake

if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR must be set")
endif()

set(lists_file "${SOURCE_DIR}/CMakeLists.txt")
set(guard "if(CMAKE_CROSSCOMPILING AND UHDR_ENABLE_INSTALL)")

if(NOT EXISTS "${lists_file}")
    message(FATAL_ERROR "No CMakeLists.txt in ${SOURCE_DIR}")
endif()

file(READ "${lists_file}" contents)

string(FIND "${contents}" "${guard}" found)
if(found EQUAL -1)
    message(STATUS "libultrahdr: cross-compile install guard already patched")
    return()
endif()

string(REPLACE
    "${guard}"
    "if(FALSE) # patched: this build wants the install target, see cmake/libultrahdr.cmake"
    contents "${contents}")

file(WRITE "${lists_file}" "${contents}")
message(STATUS "libultrahdr: cross-compile install guard patched out")
