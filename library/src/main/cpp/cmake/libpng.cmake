include(ExternalProject)

ExternalProject_Add(ep_libpng
    GIT_REPOSITORY https://github.com/pnggroup/libpng
    GIT_TAG 6d2054b29e0e07f0def670e2f2683cc9691cbf67
    DEPENDS ep_zlib
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        -DZLIB_ROOT:STRING=${CMAKE_BINARY_DIR}/fakeroot
        -DPNG_SHARED=OFF
        -DPNG_TESTS=OFF
        -DPNG_TOOLS=OFF
        "-DCMAKE_BUILD_TYPE=Release"
)
