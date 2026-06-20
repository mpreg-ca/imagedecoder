include(ExternalProject)

ExternalProject_Add(ep_highway
    GIT_REPOSITORY https://github.com/google/highway
    GIT_TAG 1.4.0
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        -DHWY_ENABLE_CONTRIB=OFF
        -DHWY_ENABLE_EXAMPLES=OFF
        -DHWY_ENABLE_TESTS=OFF
        -DHWY_SYSTEM_GTEST=ON
        -DHWY_FORCE_STATIC_LIBS=ON
)
