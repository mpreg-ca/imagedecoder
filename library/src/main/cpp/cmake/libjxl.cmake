include(ExternalProject)

ExternalProject_Add(ep_libjxl
    GIT_REPOSITORY https://github.com/libjxl/libjxl
    GIT_TAG v0.11.0
    DEPENDS ep_lcms2 ep_brotli ep_highway
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
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
        -DJPEGXL_FORCE_SYSTEM_HWY=true
)
