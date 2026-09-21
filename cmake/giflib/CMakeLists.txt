cmake_minimum_required(VERSION 3.22)
project(giflib C)

# giflib ships only a Makefile, which needs make on PATH and produces a GNU
# archive - neither works under MSVC. Seven source files, so built here
# instead with one build system for every platform.
#
# File list is giflib's own SOURCES; its tools are not built, unused here.
include(GNUInstallDirs)

add_library(gif STATIC
    ${GIFLIB_SOURCE_DIR}/dgif_lib.c
    ${GIFLIB_SOURCE_DIR}/egif_lib.c
    ${GIFLIB_SOURCE_DIR}/gifalloc.c
    ${GIFLIB_SOURCE_DIR}/gif_err.c
    ${GIFLIB_SOURCE_DIR}/gif_font.c
    ${GIFLIB_SOURCE_DIR}/gif_hash.c
    ${GIFLIB_SOURCE_DIR}/openbsd-reallocarray.c
    ${GIFLIB_SOURCE_DIR}/quantize.c
)

target_include_directories(gif PRIVATE ${GIFLIB_SOURCE_DIR})
set_target_properties(gif PROPERTIES OUTPUT_NAME gif POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    # giflib uses POSIX unsuffixed names MSVC deprecates, flooding the log otherwise.
    target_compile_definitions(gif PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE)
endif()

install(TARGETS gif ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES ${GIFLIB_SOURCE_DIR}/gif_lib.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
