include(ExternalProject)

# libvips reads gainmaps only through libuhdr: uhdrload is the loader that attaches the
# map and its metadata, and it compiles away entirely without this. uhdr2scRGB, which
# applies the map, is built unconditionally - so this dependency is what turns gainmap
# support on, not the vips side.
#
# UHDR_BUILD_DEPS stays off so it links our libjpeg-turbo rather than fetching its own.
#
# The patch re-enables the install target, which upstream disables for any cross build - see
# patches/libultrahdr-cross-install.cmake. Without it there is no libuhdr.pc, and without that
# libvips cannot find libuhdr at all.
ExternalProject_Add(ep_libultrahdr
    GIT_REPOSITORY https://github.com/google/libultrahdr
    GIT_TAG v2.0.2
    DEPENDS ep_libjpeg-turbo
    PATCH_COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=<SOURCE_DIR>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/patches/libultrahdr-cross-install.cmake
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        -DUHDR_BUILD_DEPS=0
        -DUHDR_BUILD_EXAMPLES=0
        -DUHDR_BUILD_TESTS=0
        -DUHDR_BUILD_BENCHMARK=0
        -DUHDR_BUILD_FUZZERS=0
        -DUHDR_BUILD_JAVA=0
        -DUHDR_ENABLE_INSTALL=1
        -DUHDR_ENABLE_LOGS=0
        -DUHDR_ENABLE_INTRINSICS=1
        # 2.0 can decode UltraHDR out of HEIF/AVIF, but only against a libheif carrying the
        # unmerged ISO 21496-1 patch - which it will go and build for itself if asked. Off, so
        # this stays one self-contained dependency: AVIF gain maps are a separate decision, and
        # having two libheifs in the link would be a worse problem than not having them.
        -DUHDR_ENABLE_HEIF=0
        -DUHDR_ENABLE_GLES=0
)
