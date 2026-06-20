include(ExternalProject)

ExternalProject_Add(ep_libexpat
    GIT_REPOSITORY https://github.com/libexpat/libexpat
    GIT_TAG R_2_8_1
    SOURCE_SUBDIR expat
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        -DEXPAT_BUILD_TOOLS=OFF
        -DEXPAT_BUILD_EXAMPLES=OFF
        -DEXPAT_BUILD_TESTS=OFF
)
