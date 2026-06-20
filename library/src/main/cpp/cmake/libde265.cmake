include(ExternalProject)

ExternalProject_Add(ep_libde265
    GIT_REPOSITORY https://github.com/strukturag/libde265
    GIT_TAG v1.1.1
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        -DENABLE_SDL=OFF
)
