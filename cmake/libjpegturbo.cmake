include(ExternalProject)

if(IMAGEDECODER_NO_NATIVE_SIMD)
    set(JPEG_SIMD_ARGS -DWITH_SIMD=0)
else()
    set(JPEG_SIMD_ARGS -DREQUIRE_SIMD=1)
endif()

ExternalProject_Add(ep_libjpegturbo
    GIT_REPOSITORY https://github.com/libjpeg-turbo/libjpeg-turbo
    GIT_TAG 3.2.0
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        -DWITH_JPEG8=1
        -DWITH_CRT_DLL=1
        -DWITH_TURBOJPEG=0
        -DENABLE_SHARED=0
        -DENABLE_STATIC=1
        ${JPEG_SIMD_ARGS}
        -DWITH_TOOLS=OFF
        -DWITH_TESTS=OFF
)
