include(ExternalProject)

set(ENV{PKG_CONFIG_PATH} "${THIRD_PARTY_LIB_PATH}/lib/pkgconfig")

ExternalProject_Add(ep_libheif
    GIT_REPOSITORY https://github.com/strukturag/libheif
    GIT_TAG v1.23.0
    DEPENDS ep_zlib ep_libde265 ep_dav1d ep_libwebp
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        -DWITH_EXAMPLES=OFF
        -DWITH_EXAMPLE_HEIF_THUMB=OFF
        -DWITH_EXAMPLE_HEIF_VIEW=OFF
        -DWITH_GDK_PIXBUF=OFF
        -DBUILD_DOCUMENTATION=OFF
        -DENABLE_PLUGIN_LOADING=OFF
        -DWITH_DAV1D=ON
        -DWITH_LIBDE265=ON
        "-DLIBSHARPYUV_INCLUDE_DIR=${THIRD_PARTY_LIB_PATH}/include/webp"
        "-DLIBSHARPYUV_LIBRARY=${THIRD_PARTY_LIB_PATH}/lib"
)
