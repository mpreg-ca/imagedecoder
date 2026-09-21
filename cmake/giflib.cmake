include(ExternalProject)

ExternalProject_Add(ep_giflib
    GIT_REPOSITORY https://git.code.sf.net/p/giflib/code
    GIT_TAG 6.1.3
    SOURCE_SUBDIR ""
    PATCH_COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${PROJECT_SOURCE_DIR}/cmake/giflib/CMakeLists.txt <SOURCE_DIR>/CMakeLists.txt
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        ${EP_SIZE_ARGS}
        "-DGIFLIB_SOURCE_DIR=<SOURCE_DIR>"
)
