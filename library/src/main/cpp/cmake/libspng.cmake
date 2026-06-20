include(ExternalProject)

ExternalProject_Add(ep_libspng
    GIT_REPOSITORY https://github.com/randy408/libspng
    GIT_TAG v0.7.4
    DEPENDS ep_zlib
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        -DZLIB_ROOT:STRING=${CMAKE_BINARY_DIR}/fakeroot
        -DBUILD_EXAMPLES=OFF
)
