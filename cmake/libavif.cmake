include(ExternalProject)

set(ENV{PKG_CONFIG_PATH} "${THIRD_PARTY_LIB_PATH}/lib/pkgconfig")

# AVIF decoding only, through dav1d: streams and decodes progressive layers,
# which libheif does not.
ExternalProject_Add(ep_libavif
    GIT_REPOSITORY https://github.com/AOMediaCodec/libavif
    GIT_TAG v1.4.2
    DEPENDS ep_dav1d
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        -DAVIF_CODEC_DAV1D=SYSTEM
        -DAVIF_CODEC_AOM=OFF
        -DAVIF_CODEC_RAV1E=OFF
        -DAVIF_CODEC_SVT=OFF
        -DAVIF_CODEC_LIBGAV1=OFF
        -DAVIF_LIBYUV=OFF
        -DAVIF_LIBSHARPYUV=OFF
        -DAVIF_LIBXML2=OFF
        -DAVIF_JPEG=OFF
        -DAVIF_ZLIBPNG=OFF
        -DAVIF_BUILD_APPS=OFF
        -DAVIF_BUILD_TESTS=OFF
        -DAVIF_BUILD_EXAMPLES=OFF
)
