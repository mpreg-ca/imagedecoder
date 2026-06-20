include(ExternalProject)

ExternalProject_Add(ep_libwebp
    GIT_REPOSITORY https://chromium.googlesource.com/webm/libwebp
    GIT_TAG 1.6.0
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        -DWEBP_LINK_STATIC=ON
        -DWEBP_BUILD_ANIM_UTILS=OFF
        -DWEBP_BUILD_CWEBP=OFF
        -DWEBP_BUILD_DWEBP=OFF
        -DWEBP_BUILD_GIF2WEBP=OFF
        -DWEBP_BUILD_IMG2WEBP=OFF
        -DWEBP_BUILD_VWEBP=OFF
        -DWEBP_BUILD_WEBPINFO=OFF
        -DWEBP_BUILD_WEBPMUX=OFF
        -DWEBP_BUILD_EXTRAS=OFF
        -DWEBP_BUILD_WEBP_JS=OFF
        -DWEBP_ENABLE_SWAP_16BIT_CSP=ON
)
