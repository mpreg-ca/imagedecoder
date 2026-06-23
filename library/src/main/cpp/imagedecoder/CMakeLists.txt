cmake_minimum_required(VERSION 3.22.1)

project(imagedecoder2)

add_library(imagedecoder2 SHARED
    imagedecoder.cpp
)

target_link_libraries(imagedecoder2 android jnigraphics log)
target_sources(imagedecoder2 PRIVATE imagedecoder.cpp)

find_package(PkgConfig REQUIRED)

set(ENV{PKG_CONFIG_PATH} "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig")

pkg_search_module(VIPS REQUIRED vips-cpp)

list(REMOVE_ITEM VIPS_STATIC_LIBRARIES "c++")
list(REMOVE_ITEM VIPS_STATIC_LIBRARIES "pthread")
# list(REMOVE_ITEM VIPS_STATIC_LIBRARIES "spng")
# list(APPEND VIPS_STATIC_LIBRARIES "spng_static")

target_include_directories(imagedecoder2 PRIVATE ${VIPS_STATIC_INCLUDE_DIRS})
target_link_directories(imagedecoder2 PRIVATE ${VIPS_STATIC_LIBRARY_DIRS})
target_link_libraries(imagedecoder2 vips-cpp ${VIPS_STATIC_LIBRARIES} -static-libgcc -static-libstdc++)
