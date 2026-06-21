include(ExternalProject)

ExternalProject_Add(ep_libvips
    GIT_REPOSITORY https://github.com/libvips/libvips
    GIT_TAG v8.18.3
    PATCH_COMMAND git apply ${CMAKE_CURRENT_SOURCE_DIR}/patches/libvips-remove-subdirs.patch || true
    DEPENDS ep_libexpat ep_glib ep_highway ep_lcms2 ep_libpng ep_libjpeg-turbo ep_libopenjp2 ep_libwebp ep_libtiff ep_libheif ep_libjxl
    CONFIGURE_COMMAND
        ${Meson_EXECUTABLE} setup
        ${EP_MESON_ARGS}
        -Ddeprecated=false
        -Dexamples=false
        -Dcplusplus=true
        -Dmodules=disabled
        -Dintrospection=disabled
        -Dfuzzing_engine=none
        -Djpeg-xl-module=disabled
        <BINARY_DIR> <SOURCE_DIR>
    BUILD_COMMAND ${Meson_EXECUTABLE} compile -C <BINARY_DIR>
    INSTALL_COMMAND ${Meson_EXECUTABLE} install -C <BINARY_DIR>
)
