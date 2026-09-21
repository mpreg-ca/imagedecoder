include(ExternalProject)

# libjxl builds with -mrelax-all, which drops Thumb cbz/cbnz branches.
set(LIBJXL_ARM_ARGS)
if(ANDROID AND ANDROID_ABI STREQUAL "armeabi-v7a")
    set(LIBJXL_ARM_ARGS -DANDROID_ARM_MODE=arm)
endif()

ExternalProject_Add(ep_libjxl
    GIT_REPOSITORY https://github.com/libjxl/libjxl
    GIT_TAG v0.12.0
    DEPENDS ep_lcms2 ep_brotli ep_highway
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        ${LIBJXL_ARM_ARGS}
        -DJPEGXL_ENABLE_TOOLS=false
        -DJPEGXL_ENABLE_DOXYGEN=false
        -DJPEGXL_ENABLE_MANPAGES=false
        -DJPEGXL_ENABLE_BENCHMARK=false
        -DJPEGXL_ENABLE_EXAMPLES=false
        -DJPEGXL_BUNDLE_LIBPNG=false
        -DJPEGXL_ENABLE_JNI=false
        -DJPEGXL_ENABLE_SJPEG=false
        -DJPEGXL_ENABLE_OPENEXR=false
        -DJPEGXL_ENABLE_SKCMS=false
        -DJPEGXL_ENABLE_TRANSCODE_JPEG=false
        -DJPEGXL_FORCE_SYSTEM_BROTLI=true
        -DJPEGXL_FORCE_SYSTEM_LCMS2=true
        "-DLCMS2_INCLUDE_DIR=${THIRD_PARTY_LIB_PATH}/include"
        "-DLCMS2_LIBRARY=${LCMS2_STATIC_LIBRARY}"
        -DJPEGXL_FORCE_SYSTEM_HWY=true
    # The decoder library only; configure still wants brotlienc to exist.
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config ${CMAKE_BUILD_TYPE}
        --target jxl_dec jxl_threads
    INSTALL_COMMAND ${CMAKE_COMMAND} -DBIN=<BINARY_DIR> -DPREFIX=${THIRD_PARTY_LIB_PATH}
        -P ${PROJECT_SOURCE_DIR}/cmake/libjxl-dec-install.cmake
)
