include(ExternalProject)

ExternalProject_Add(ep_libopenjp2
    GIT_REPOSITORY https://github.com/uclouvain/openjpeg
    GIT_TAG 919663142e549b1f339da4ad12949da85e91e1d9
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        -DBUILD_SHARED_LIBS=OFF
        -DBUILD_CODEC=OFF
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
)
