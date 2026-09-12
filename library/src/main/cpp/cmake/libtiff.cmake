include(ExternalProject)

ExternalProject_Add(ep_libtiff
    GIT_REPOSITORY https://github.com/libsdl-org/libtiff
    GIT_TAG v4.7.2
    DEPENDS ep_zlib ep_libjpeg-turbo
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        -DZLIB_ROOT:STRING=${CMAKE_BINARY_DIR}/fakeroot
        -Djpeg=ON
        -Djbig=OFF
        -Dlzma=OFF
        -Dlerc=OFF
        -Dlibdeflate=OFF
        -Dtiff-tools=OFF
        -Dtiff-tests=OFF
        -Dtiff-contrib=OFF
        -Dtiff-docs=OFF
)
