include(ExternalProject)

ExternalProject_Add(ep_libheif
    GIT_REPOSITORY https://github.com/strukturag/libheif
    GIT_TAG v1.19.2
    DEPENDS ep_zlib ep_libde265 ep_dav1d
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        -DWITH_EXAMPLES=OFF
        -DWITH_EXAMPLE_HEIF_THUMB=OFF
        -DWITH_EXAMPLE_HEIF_VIEW=OFF
        -DWITH_GDK_PIXBUF=OFF
        -DBUILD_DOCUMENTATION=OFF
)
