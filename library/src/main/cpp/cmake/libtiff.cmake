include(ExternalProject)

ExternalProject_Add(ep_libtiff
    GIT_REPOSITORY https://github.com/libsdl-org/libtiff
    GIT_TAG v4.7.1
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
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
