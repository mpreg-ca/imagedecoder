include(ExternalProject)

ExternalProject_Add(ep_brotli
    GIT_REPOSITORY https://github.com/google/brotli
    GIT_TAG v1.2.0
    CMAKE_ARGS ${EP_CMAKE_ARGS}
)
