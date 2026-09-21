# Order is dependent-before-dependency, required for a static link.
file(GLOB IMAGEDECODER_VERSIONED_INCLUDES
     "${IMAGEDECODER_PREFIX}/include/openjpeg-*"
     "${IMAGEDECODER_PREFIX}/include/libpng*")

set(IMAGEDECODER_INCLUDE_DIRS
    ${IMAGEDECODER_PREFIX}/include/imagedecoder
    ${IMAGEDECODER_PREFIX}/include
    ${IMAGEDECODER_VERSIONED_INCLUDES}
)

set(IMAGEDECODER_LINK_LIBS
    jxl_dec
    jxl_threads
    hwy
    brotlidec
    brotlicommon

    heif
    avif
    dav1d
    de265

    openjp2

    uhdr

    webpdemux
    webpdecoder

    gif
    tiff
    png18
    jpeg
    lcms2
    z
)
