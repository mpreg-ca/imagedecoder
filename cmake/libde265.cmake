include(ExternalProject)

if(IMAGEDECODER_NO_NATIVE_SIMD)
    set(DE265_SIMD_ARGS -DENABLE_SIMD=OFF)
else()
    set(DE265_SIMD_ARGS)
endif()

ExternalProject_Add(ep_libde265
    GIT_REPOSITORY https://github.com/strukturag/libde265
    GIT_TAG v1.1.3
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        -DENABLE_SDL=OFF
        -DENABLE_DECODER=OFF
        ${DE265_SIMD_ARGS}
)
