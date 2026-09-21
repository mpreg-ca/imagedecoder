include(ExternalProject)

if(IMAGEDECODER_NO_NATIVE_SIMD)
    set(DAV1D_SIMD_ARGS -Denable_asm=false)
else()
    set(DAV1D_SIMD_ARGS)
endif()

ExternalProject_Add(ep_dav1d
    GIT_REPOSITORY https://code.videolan.org/videolan/dav1d
    GIT_TAG 1.5.4
    CONFIGURE_COMMAND ${Meson_EXECUTABLE} setup ${EP_MESON_ARGS}
        -Denable_tools=false -Denable_tests=false
        ${DAV1D_SIMD_ARGS}
        <BINARY_DIR> <SOURCE_DIR>
    BUILD_COMMAND ${Meson_EXECUTABLE} compile -j ${NPROC} -C <BINARY_DIR>
    INSTALL_COMMAND ${Meson_EXECUTABLE} install -C <BINARY_DIR>
)
