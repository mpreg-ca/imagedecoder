include(ExternalProject)

if(IMAGEDECODER_NO_NATIVE_SIMD)
    set(UHDR_SIMD_ARGS -DUHDR_ENABLE_INTRINSICS=0)
else()
    set(UHDR_SIMD_ARGS -DUHDR_ENABLE_INTRINSICS=1)
endif()

ExternalProject_Add(ep_libultrahdr
    GIT_REPOSITORY https://github.com/google/libultrahdr
    GIT_TAG v2.0.2
    DEPENDS ep_libjpegturbo
    PATCH_COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=<SOURCE_DIR>
        -P ${PROJECT_SOURCE_DIR}/cmake/libultrahdr-cross-install.cmake
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        ${EP_SIZE_ARGS}
        -DUHDR_BUILD_DEPS=0
        -DUHDR_BUILD_EXAMPLES=0
        -DUHDR_BUILD_TESTS=0
        -DUHDR_BUILD_BENCHMARK=0
        -DUHDR_BUILD_FUZZERS=0
        -DUHDR_BUILD_JAVA=0
        -DUHDR_ENABLE_INSTALL=1
        -DUHDR_ENABLE_LOGS=0
        ${UHDR_SIMD_ARGS}
        -DUHDR_ENABLE_HEIF=0
        -DUHDR_ENABLE_GLES=0
        -DBUILD_FOR_WINUI=ON
)
