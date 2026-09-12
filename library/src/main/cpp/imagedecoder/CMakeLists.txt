cmake_minimum_required(VERSION 3.22.1)

project(imagedecoder2)

add_library(imagedecoder2 SHARED
    imagedecoder.cpp
)

target_link_libraries(imagedecoder2 android jnigraphics log)
target_sources(imagedecoder2 PRIVATE imagedecoder.cpp)

find_package(PkgConfig REQUIRED)

set(ENV{PKG_CONFIG_PATH} "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig")

pkg_check_modules(VIPS REQUIRED vips-cpp)

list(REMOVE_ITEM VIPS_STATIC_LIBRARIES "c++")
list(REMOVE_ITEM VIPS_STATIC_LIBRARIES "pthread")

target_include_directories(imagedecoder2 PRIVATE ${VIPS_STATIC_INCLUDE_DIRS} ${CMAKE_INSTALL_PREFIX}/include)
target_link_directories(imagedecoder2 PRIVATE ${VIPS_STATIC_LIBRARY_DIRS})

# uhdr, dav1d and jxl are named explicitly rather than left to VIPS_STATIC_LIBRARIES: pkg-config only
# resolves them into that list if vips.pc already required them when this project configured, and
# its cache outlives a libvips rebuild that adds one. Listed after the vips libs so the static
# link order still runs dependent-before-dependency.
target_link_libraries(imagedecoder2
    vips-cpp
    ${VIPS_STATIC_LIBRARIES}
    dav1d
    uhdr
    jxl
    exif
    tiff
    -static-libgcc
    -static-libstdc++
)
