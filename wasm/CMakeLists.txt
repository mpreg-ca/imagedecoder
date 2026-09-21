cmake_minimum_required(VERSION 3.22.1)

project(imagedecoder_wasm CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# imagedecoder.js (an ES module factory) and imagedecoder.wasm; the
# single-threaded build is imagedecoder-nothreads, so both can ship side by side.
if(NOT DEFINED IMAGEDECODER_WASM_THREADS)
    set(IMAGEDECODER_WASM_THREADS ON)
endif()
if(IMAGEDECODER_WASM_THREADS)
    set(_name imagedecoder)
else()
    set(_name imagedecoder-nothreads)
endif()

add_executable(imagedecoder_js imagedecoder_wasm.cpp)
set_target_properties(imagedecoder_js PROPERTIES OUTPUT_NAME ${_name} SUFFIX ".js")
if(NOT IMAGEDECODER_WASM_THREADS)
    target_compile_definitions(imagedecoder_js PRIVATE IMAGEDECODER_SINGLE_THREADED)
endif()

find_package(PkgConfig REQUIRED)
set(ENV{PKG_CONFIG_PATH} "${IMAGEDECODER_PREFIX}/lib/pkgconfig")
set(ENV{PKG_CONFIG_LIBDIR} "${IMAGEDECODER_PREFIX}/lib/pkgconfig")
set(PKG_CONFIG_ARGN --static)

# Confirms the library is installed and gives its include path; link list is the shared one.
pkg_check_modules(IMAGEDECODER REQUIRED imagedecoder)
include(${IMAGEDECODER_CMAKE_DIR}/link-libs.cmake)

target_include_directories(imagedecoder_js PRIVATE ${IMAGEDECODER_INCLUDE_DIRS})
target_link_directories(imagedecoder_js PRIVATE ${IMAGEDECODER_PREFIX}/lib)
target_link_libraries(imagedecoder_js imagedecoder ${IMAGEDECODER_LINK_LIBS})

target_link_options(imagedecoder_js PRIVATE
    "-lembind"
    "-sMODULARIZE=1"
    "-sEXPORT_ES6=1"
    "-sEXPORT_NAME=createImageDecoderModule"
    "-sENVIRONMENT=web,worker,node"
    # Pages can be hundreds of megabytes decoded.
    "-sALLOW_MEMORY_GROWTH=1"
    "-sMAXIMUM_MEMORY=4GB"
    # Emscripten's 64 KB default overflows in dav1d and libjxl.
    "-sSTACK_SIZE=4MB"
)
if(IMAGEDECODER_WASM_THREADS)
    target_link_options(imagedecoder_js PRIVATE
        # dav1d and libjxl each keep two threads per live decoder, from a pool
        # made up front: a worker can't be spawned while the main thread waits.
        # Past the pool, thread creation fails (strict) rather than hanging, and
        # the decoders carry on single-threaded.
        "-sPTHREAD_POOL_SIZE=8"
        "-sPTHREAD_POOL_SIZE_STRICT=2"
        "-sDEFAULT_PTHREAD_STACK_SIZE=2MB"
    )
endif()

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/${_name}.js
    ${CMAKE_CURRENT_BINARY_DIR}/${_name}.wasm
    DESTINATION wasm)
