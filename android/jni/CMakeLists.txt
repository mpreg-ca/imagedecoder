cmake_minimum_required(VERSION 3.22.1)

project(imagedecoder_jni CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(imagedecoder_jni SHARED imagedecoder_jni.cpp)

# The .so is almost all statically linked third-party code, and by default
# all ~9000 symbols export - costing symbol tables/relocations directly, and
# more indirectly since an exported symbol is a --gc-sections root. The
# version script below limits the surface to JNI entry points; --exclude-libs
# does the same for static-archive symbols.
target_compile_options(imagedecoder_jni PRIVATE
    -fvisibility=hidden
    -fvisibility-inlines-hidden
    -ffunction-sections
    -fdata-sections
)

target_link_options(imagedecoder_jni PRIVATE
    "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/imagedecoder.map"
    "-Wl,--exclude-libs,ALL"
    "-Wl,--gc-sections"
    # Folds identical machine code; codecs carry many near-duplicate SIMD kernels.
    "-Wl,--icf=all"
)

set_target_properties(imagedecoder_jni PROPERTIES
    LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/imagedecoder.map)

# Named apart from OUTPUT_NAME so it doesn't collide with the core static
# library in the same fakeroot; System.loadLibrary asks for OUTPUT_NAME.
set_target_properties(imagedecoder_jni PROPERTIES OUTPUT_NAME imagedecoder)

include(${IMAGEDECODER_CMAKE_DIR}/link-libs.cmake)

find_library(IMAGEDECODER_CORE imagedecoder
    PATHS ${IMAGEDECODER_PREFIX}/lib NO_DEFAULT_PATH REQUIRED)

target_include_directories(imagedecoder_jni PRIVATE ${IMAGEDECODER_INCLUDE_DIRS})

target_link_directories(imagedecoder_jni PRIVATE ${IMAGEDECODER_PREFIX}/lib)

target_link_libraries(imagedecoder_jni
    ${IMAGEDECODER_CORE}
    ${IMAGEDECODER_LINK_LIBS}

    log
    m
    -static-libgcc
    -static-libstdc++
)
