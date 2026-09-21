cmake_minimum_required(VERSION 3.22.1)

project(imagedecoder VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(imagedecoder STATIC
    decoder_base.cpp
    decoder_png.cpp
    decoder_jpeg.cpp
    decoder_tiff.cpp
    decoder_webp.cpp
    decoder_gif.cpp
    decoder_heif.cpp
    decoder_avif.cpp
    decoder_jxl.cpp
    decoder_jp2.cpp
)
add_library(imagedecoder::imagedecoder ALIAS imagedecoder)

if(EMSCRIPTEN AND DEFINED IMAGEDECODER_WASM_THREADS AND NOT IMAGEDECODER_WASM_THREADS)
    target_compile_definitions(imagedecoder PUBLIC IMAGEDECODER_SINGLE_THREADED)
endif()

target_include_directories(imagedecoder PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include/imagedecoder>
)

# lcms2, libpng and libjpeg-turbo each install a .pc file, so one mechanism covers all three.
find_package(PkgConfig REQUIRED)

# Under the superbuild, dependencies live in the fakeroot prefix, not the system; for a
# cross build (Android) the system ones must not be found at all.
if(IMAGEDECODER_PREFIX)
    if(CMAKE_CROSSCOMPILING)
        set(ENV{PKG_CONFIG_LIBDIR} "${IMAGEDECODER_PREFIX}/lib/pkgconfig")
        set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
    endif()
    set(ENV{PKG_CONFIG_PATH} "${IMAGEDECODER_PREFIX}/lib/pkgconfig")

    # Everything in that prefix is a static archive, so consumers need the private
    # dependency chains too (e.g. libpng's Requires.private: zlib, or inflate/deflate is
    # undefined when the .so links). pkg_check_modules' IMPORTED_TARGET always uses the
    # shared-link variables, so the _STATIC_ ones are applied by hand.
    set(PKG_CONFIG_ARGN --static)
    set(PKG "_STATIC")
endif()

# One list, used to find the packages, link them, and write imagedecoder.pc for consumers
# outside this build - so a format added to one spot can't be missed in another.
#
# Dependent-before-dependency order, required for a static link: each archive resolves
# once, left to right, so lcms2 (called into, calls into nothing) comes last.
set(IMAGEDECODER_DEPS
    libuhdr
    libopenjp2
    libjxl_dec
    libjxl_threads
    libheif
    libavif
    libwebpdemux
    libwebpdecoder
    libtiff-4
    libpng
    libjpeg
    lcms2
)

# giflib ships no .pc file, so located by hand and linked first (libheif's AVIF path
# doesn't call into it).
find_library(GIF_LIBRARY NAMES gif PATHS ${IMAGEDECODER_PREFIX}/lib REQUIRED)
find_path(GIF_INCLUDE_DIR gif_lib.h PATHS ${IMAGEDECODER_PREFIX}/include REQUIRED)
target_include_directories(imagedecoder PUBLIC ${GIF_INCLUDE_DIR})
target_link_libraries(imagedecoder PUBLIC ${GIF_LIBRARY})

# libpng writes no .pc on Windows, so there it joins giflib. Its full path
# goes into the .pc below: -l would have to guess how a consumer spells
# libpng18_static.lib.
if(WIN32)
    list(REMOVE_ITEM IMAGEDECODER_DEPS libpng)
    find_library(PNG_LIBRARY NAMES png18_static png18 png
                 PATHS ${IMAGEDECODER_PREFIX}/lib REQUIRED)
    find_path(PNG_INCLUDE_DIR png.h PATHS ${IMAGEDECODER_PREFIX}/include REQUIRED)
    target_include_directories(imagedecoder PUBLIC ${PNG_INCLUDE_DIR})
    target_link_libraries(imagedecoder PUBLIC ${PNG_LIBRARY})
endif()

foreach(_mod IN LISTS IMAGEDECODER_DEPS)
    string(MAKE_C_IDENTIFIER "${_mod}" _var)
    string(TOUPPER "${_var}" _var)
    pkg_check_modules(${_var} REQUIRED ${_mod})
    target_include_directories(imagedecoder PUBLIC ${${_var}${PKG}_INCLUDE_DIRS})
    target_link_directories(imagedecoder PUBLIC ${${_var}${PKG}_LIBRARY_DIRS})
    target_link_libraries(imagedecoder PUBLIC ${${_var}${PKG}_LIBRARIES})
    # Under --static this carries Cflags.private: the static-library defines.
    target_compile_options(imagedecoder PUBLIC ${${_var}${PKG}_CFLAGS_OTHER})
endforeach()

# So a consumer outside this build can find and link the library.
string(REPLACE ";" " " IMAGEDECODER_PC_REQUIRES "${IMAGEDECODER_DEPS}")
# Named by hand: giflib ships no .pc at all. Same reasoning as the Android link line.
set(IMAGEDECODER_PC_LIBS_PRIVATE "-lgif -ldav1d -lde265")
# MSVC's C++ runtime is implicit; there is no stdc++.lib to name.
if(NOT MSVC)
    string(APPEND IMAGEDECODER_PC_LIBS_PRIVATE " -lstdc++")
endif()
if(WIN32)
    string(APPEND IMAGEDECODER_PC_LIBS_PRIVATE " ${PNG_LIBRARY}")
endif()
configure_file(imagedecoder.pc.in imagedecoder.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/imagedecoder.pc
        DESTINATION lib/pkgconfig)

install(TARGETS imagedecoder ARCHIVE DESTINATION lib)

# Globbed, not listed: a header forgotten here compiles fine in-tree but fails for
# every consumer (has happened twice). CONFIGURE_DEPENDS re-globs when the set changes.
file(GLOB IMAGEDECODER_HEADERS CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
install(FILES ${IMAGEDECODER_HEADERS} DESTINATION include/imagedecoder)
