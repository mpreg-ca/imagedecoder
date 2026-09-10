include(ExternalProject)

ExternalProject_Add(ep_libexif
    GIT_REPOSITORY https://github.com/libexif/libexif
    GIT_TAG v0.6.26
    DEPENDS ep_libiconv
    BUILD_IN_SOURCE true
    CONFIGURE_COMMAND
        autoreconf -fiv <SOURCE_DIR> &&
        <SOURCE_DIR>/configure ${EP_AUTOTOOLS_ARGS} --disable-docs --disable-nls
    BUILD_COMMAND ${Make_EXECUTABLE} -j${NPROC}
    INSTALL_COMMAND ${Make_EXECUTABLE} install
)
