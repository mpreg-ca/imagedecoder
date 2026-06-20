include(ExternalProject)

ExternalProject_Add(ep_glib
    URL https://download.gnome.org/sources/glib/2.89/glib-2.89.0.tar.xz
    URL_HASH SHA256=205bf5dab175de68f11e33be7bb36d4ad4c5a5097d8c0c88a8682b257b6293dc
    DEPENDS ep_zlib ep_libffi ep_libiconv
    CONFIGURE_COMMAND
        ${Meson_EXECUTABLE} setup
        ${EP_MESON_ARGS}
        -Dtests=false
        <BINARY_DIR> <SOURCE_DIR>
    BUILD_COMMAND ${Meson_EXECUTABLE} compile -C <BINARY_DIR>
    INSTALL_COMMAND ${Meson_EXECUTABLE} install -C <BINARY_DIR>
)
