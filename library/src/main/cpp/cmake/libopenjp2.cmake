include(ExternalProject)

ExternalProject_Add(ep_libopenjp2
    GIT_REPOSITORY https://github.com/uclouvain/openjpeg
    GIT_TAG v2.5.4
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_CODEC=OFF
)
