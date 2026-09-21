include(ExternalProject)

if(IMAGEDECODER_NO_NATIVE_SIMD)
    set(PNG_SIMD_ARGS -DPNG_HARDWARE_OPTIMIZATIONS=OFF)
else()
    set(PNG_SIMD_ARGS)
endif()

ExternalProject_Add(ep_libpng
    GIT_REPOSITORY https://github.com/pnggroup/libpng
    GIT_TAG d1d0abeffede1cc898ddc3d0e600839cf026d749
    DEPENDS ep_zlib
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        -DZLIB_ROOT:STRING=${CMAKE_BINARY_DIR}/fakeroot
        -DPNG_SHARED=OFF
        -DPNG_TESTS=OFF
        -DPNG_TOOLS=OFF
        ${PNG_SIMD_ARGS}
        "-DCMAKE_BUILD_TYPE=Release"
)
