# libultrahdr switches its install target off whenever CMAKE_CROSSCOMPILING is set, which for an
# Android build is always - so `cmake --build . --target install` fails with "unknown target
# 'install'" and ExternalProject's install step dies with it.
#
# We do want that install: it is what puts libuhdr.a, ultrahdr_api.h and libuhdr.pc into the
# fakeroot, and libuhdr.pc is how libvips's `dependency('libuhdr')` finds any of it. Upstream
# disables it to stop a cross build landing in a system prefix; ours installs into a private
# fakeroot, so the reason doesn't apply.
#
# A CMake script rather than a .patch file for two reasons: it is idempotent, so it survives the
# patch step running again on an already-patched checkout, and it matches a single line, so it
# doesn't care whether the checkout has LF or CRLF endings.
#
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
