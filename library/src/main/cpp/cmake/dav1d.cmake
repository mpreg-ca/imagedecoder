include(ExternalProject)

ExternalProject_Add(ep_dav1d
    GIT_REPOSITORY https://code.videolan.org/videolan/dav1d
    GIT_TAG 1.5.3
    CONFIGURE_COMMAND ${Meson_EXECUTABLE} setup ${EP_MESON_ARGS} <BINARY_DIR> <SOURCE_DIR>
    BUILD_COMMAND ${Meson_EXECUTABLE} compile -C <BINARY_DIR>
    INSTALL_COMMAND ${Meson_EXECUTABLE} install -C <BINARY_DIR>
)
