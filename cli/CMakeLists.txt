cmake_minimum_required(VERSION 3.22.1)

project(imagedecoder_cli CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(imagedecoder-cli main.cpp)

find_package(PkgConfig REQUIRED)

if(IMAGEDECODER_PREFIX)
    set(ENV{PKG_CONFIG_PATH} "${IMAGEDECODER_PREFIX}/lib/pkgconfig")
    set(PKG_CONFIG_ARGN --static)
endif()

# Confirms the library is installed and gives its include path; link list is the shared one.
pkg_check_modules(IMAGEDECODER REQUIRED imagedecoder)
include(${IMAGEDECODER_CMAKE_DIR}/link-libs.cmake)

target_include_directories(imagedecoder-cli PRIVATE ${IMAGEDECODER_INCLUDE_DIRS})
target_link_directories(imagedecoder-cli PRIVATE ${IMAGEDECODER_PREFIX}/lib)
target_link_libraries(imagedecoder-cli imagedecoder ${IMAGEDECODER_LINK_LIBS} m)

install(TARGETS imagedecoder-cli RUNTIME DESTINATION bin)
