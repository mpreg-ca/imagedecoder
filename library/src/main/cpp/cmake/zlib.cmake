include(ExternalProject)

ExternalProject_Add(ep_zlib
    GIT_REPOSITORY https://github.com/zlib-ng/zlib-ng
    GIT_TAG 2.3.3
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        -DINSTALL_PKGCONFIG_DIR=${THIRD_PARTY_LIB_PATH}/lib/pkgconfig
        -DZLIB_COMPAT=ON
)
