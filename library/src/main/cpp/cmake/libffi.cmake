include(ExternalProject)

ExternalProject_Add(ep_libffi
    GIT_REPOSITORY https://github.com/libffi/libffi
    GIT_TAG v3.6.0
    BUILD_IN_SOURCE true
    CONFIGURE_COMMAND
        <SOURCE_DIR>/autogen.sh &&
        <SOURCE_DIR>/configure ${EP_AUTOTOOLS_ARGS}
        --disable-builddir
        --disable-multi-os-directory
        --enable-pax_emutramp
    BUILD_COMMAND ${Make_EXECUTABLE} all
    INSTALL_COMMAND ${Make_EXECUTABLE} install
)
