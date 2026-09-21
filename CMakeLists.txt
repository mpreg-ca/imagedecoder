cmake_minimum_required(VERSION 3.22.1)

# Before project(), which defaults it to Debug under MSVC.
if(NOT DEFINED CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type")
endif()

# Part of the pin hash: bump when dependency build settings change.
set(IMAGEDECODER_DEPS_REVISION 7)

project(imagedecoder)

# Threads need SharedArrayBuffer, so a cross-origin isolated page (COOP/COEP).
# OFF builds a module that runs on any page, on one thread.
option(IMAGEDECODER_WASM_THREADS "Emscripten: threaded build (needs COOP/COEP)" ON)

# Force single-config: a multi-config generator builds sub-projects as Debug
# (no inherited CMAKE_BUILD_TYPE, first configuration wins), and under MSVC
# that makes lcms2 install as lcms2d.lib, which libjxl then can't find.
# FORCE is needed because the generator already created these cache entries.
# The CMAKE_BUILD_TYPE guard preserves a caller-supplied value.
if(CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_CONFIGURATION_TYPES Release CACHE STRING "Configurations" FORCE)
endif()
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(THIRD_PARTY_LIB_PATH ${CMAKE_BINARY_DIR}/fakeroot)
set(CMAKE_FIND_ROOT_PATH ${THIRD_PARTY_LIB_PATH};${CMAKE_FIND_ROOT_PATH})
set_directory_properties(PROPERTIES EP_PREFIX ${THIRD_PARTY_LIB_PATH})

# Prebuilt dependency tree from tools/make-prebuilt.sh, in place of building
# all sixteen (~25 min). Platform/libc-specific: static archives reference
# the libc they were built against.
set(IMAGEDECODER_PREBUILT_DEPS "" CACHE PATH
    "Unpacked prebuilt dependency tree to use instead of building them")

# Or fetch it: downloads the tarball for this checkout's pins (see
# tools/fetch-prebuilt.cmake). Falls back to a source build if none exists.
option(IMAGEDECODER_FETCH_PREBUILT_DEPS
       "Download the published prebuilt dependencies for these pins" OFF)
set(IMAGEDECODER_PREBUILT_REPO "mpreg-ca/imagedecoder" CACHE STRING
    "owner/repo whose releases carry the prebuilt dependency tarballs")
set(IMAGEDECODER_PREBUILT_URL
    "https://github.com/${IMAGEDECODER_PREBUILT_REPO}/releases/download"
    CACHE STRING "Base URL for the prebuilt tarballs; override for a mirror")

if(IMAGEDECODER_FETCH_PREBUILT_DEPS AND NOT IMAGEDECODER_PREBUILT_DEPS)
    include("${PROJECT_SOURCE_DIR}/tools/fetch-prebuilt.cmake")
    imagedecoder_fetch_prebuilt(_fetched)
    if(_fetched)
        set(IMAGEDECODER_PREBUILT_DEPS "${_fetched}")
    endif()
endif()

if(IMAGEDECODER_PREBUILT_DEPS)
    set(IMAGEDECODER_BUILD_DEPS OFF)
else()
    set(IMAGEDECODER_BUILD_DEPS ON)
endif()

# A prebuilt-deps consumer needs none of this.
if(IMAGEDECODER_BUILD_DEPS)

# Every dependency builds with CMake except dav1d (meson).
find_program(Meson_EXECUTABLE meson)
if(NOT Meson_EXECUTABLE)
    message(FATAL_ERROR "Meson is required (dav1d)")
endif()

# Job count for meson (CMake dependencies inherit the outer build's).
include(ProcessorCount)
ProcessorCount(NPROC)
if(NPROC EQUAL 0)
    set(NPROC 4)
endif()

# dav1d and libjpeg-turbo each find their own assembler via PATH
# independently (dav1d asks meson; libjpeg-turbo asks CMake), so a wrong
# nasm first on PATH fails in each its own way.
# Emscripten reports an x86 processor but assembles nothing.
if(NOT ANDROID AND NOT EMSCRIPTEN AND
   (CMAKE_SYSTEM_PROCESSOR MATCHES "x86|AMD64|i.86"))
    # macOS and Windows always use a pinned nasm: macOS's /usr/bin/nasm is a
    # 2001-era 0.98, and Windows has none. Settable by hand for testing.
    if(APPLE OR WIN32)
        set(IMAGEDECODER_PIN_NASM ON)
    endif()

    if(IMAGEDECODER_PIN_NASM)
        # Version/checksums live in cmake/nasm.cmake; ep_nasm creates this file,
        # and assembling dependencies are ordered after it.
        if(WIN32)
            set(NASM_BINARY nasm.exe)
        else()
            set(NASM_BINARY nasm)
        endif()
        set(Nasm_EXECUTABLE "${THIRD_PARTY_LIB_PATH}/bin/${NASM_BINARY}")
    else()
        # Linux/Android keep the system nasm.
        #
        # No default for Nasm_EXECUTABLE: a plain set() would shadow
        # find_program's cache entry and make the search look like it failed.
        find_program(Nasm_EXECUTABLE nasm)

        if(NOT Nasm_EXECUTABLE)
            message(FATAL_ERROR
                "nasm 2.14 or newer is required for the x86 SIMD in dav1d "
                "and libjpeg-turbo, and none is on PATH.")
        endif()

        execute_process(COMMAND "${Nasm_EXECUTABLE}" -v
                        OUTPUT_VARIABLE _nasm_v ERROR_QUIET
                        OUTPUT_STRIP_TRAILING_WHITESPACE)
        if(NOT _nasm_v MATCHES "NASM version ([0-9]+)\\.([0-9]+)")
            message(FATAL_ERROR
                "${Nasm_EXECUTABLE} does not report a NASM version: "
                "'${_nasm_v}'")
        endif()
        set(_nasm_major ${CMAKE_MATCH_1})
        set(_nasm_short "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}")
        if(_nasm_major LESS 2 OR
           (_nasm_major EQUAL 2 AND CMAKE_MATCH_2 LESS 14))
            message(FATAL_ERROR
                "nasm 2.14 or newer is required for the x86 SIMD in dav1d "
                "and libjpeg-turbo, but the one on PATH is "
                "${Nasm_EXECUTABLE}, version ${_nasm_short}.")
        endif()

        message(STATUS
            "imagedecoder: nasm ${Nasm_EXECUTABLE} (${_nasm_short})")
    endif()
endif()

endif()  # IMAGEDECODER_BUILD_DEPS

# Escape ';' so these lists survive as single -D arguments
string(REPLACE ";" "$<SEMICOLON>" CMAKE_FIND_ROOT_PATH_STR "${CMAKE_FIND_ROOT_PATH}")
string(REPLACE ";" "$<SEMICOLON>" CMAKE_FIND_LIBRARY_SUFFIXES_STR "${CMAKE_FIND_LIBRARY_SUFFIXES}")

# Common args for every CMake ExternalProject
set(EP_CMAKE_ARGS
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
    "-DCMAKE_INSTALL_PREFIX=${THIRD_PARTY_LIB_PATH}"
    "-DCMAKE_INSTALL_LIBDIR=lib"
    "-DCMAKE_PREFIX_PATH=${THIRD_PARTY_LIB_PATH}"
    "-DCMAKE_FIND_ROOT_PATH=${CMAKE_FIND_ROOT_PATH_STR}"
    "-DBUILD_SHARED_LIBS=OFF"
    "-DBUILD_TESTING=OFF"
    "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
)

# Almost all of the shipped binary is these dependencies, so size flags go
# here rather than on the consumer.
#
# CMAKE_{C,CXX}_FLAGS replaces CMake's compiler defaults, so each branch must
# be complete: MSVC's default CXX flags carry /EHsc, and dropping it leaves
# the throwing dependencies (libheif, libjxl) with no unwinder (C4530).
if(MSVC)
    # /Gy and /Gw are the function-/data-section equivalents for /OPT:REF.
    # No equivalent for dropping unwind tables: Windows needs them for SEH.
    # Headers are dllimport without their static macro, which only .pc files'
    # Cflags.private carry and the dependencies' find modules never read.
    set(_static "")
    foreach(_def IN ITEMS LIBDE265_STATIC_BUILD LIBHEIF_STATIC_BUILD OPJ_STATIC
                          HWY_STATIC_DEFINE JXL_STATIC_DEFINE
                          JXL_THREADS_STATIC_DEFINE JXL_CMS_STATIC_DEFINE)
        string(APPEND _static " /D${_def}")
    endforeach()
    list(APPEND EP_CMAKE_ARGS
        "-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS /Gy /Gw${_static}"
        "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /GR /EHsc /Gy /Gw${_static}"
        # One DLL CRT; CMP0091 so older projects honour it.
        "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW"
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL"
    )
else()
    # Function/data sections give --gc-sections something to collect:
    # otherwise an archive member is all-or-nothing.
    #
    # Unwind tables dropped for C only: libpng/libjpeg suspend via longjmp
    # (no unwinding involved). C++ dependencies keep theirs for exceptions.
    set(_flags "-ffunction-sections -fdata-sections")
    set(_c_extra "-fno-asynchronous-unwind-tables -fno-unwind-tables")
    if(EMSCRIPTEN)
        # Every object alike, or the link refuses: shared memory for threads
        # (or none), WASM SIMD, and one exception/longjmp ABI (the decoders
        # throw; libpng and libjpeg longjmp).
        if(IMAGEDECODER_WASM_THREADS)
            string(APPEND _flags " -pthread")
        endif()
        string(APPEND _flags " -msimd128 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm")
        set(_c_extra "")
    endif()
    list(APPEND EP_CMAKE_ARGS
        "-DCMAKE_C_FLAGS=${_flags} ${_c_extra}"
        "-DCMAKE_CXX_FLAGS=${_flags}"
    )
endif()

# For dependencies off the pixel path (container parsing, metadata, rare
# formats): size over speed. The rest keep the build type's optimisation.
if(MSVC)
    set(EP_SIZE_ARGS
        "-DCMAKE_C_FLAGS_RELEASE=/O1 /Ob1 /DNDEBUG"
        "-DCMAKE_CXX_FLAGS_RELEASE=/O1 /Ob1 /DNDEBUG")
else()
    set(EP_SIZE_ARGS
        "-DCMAKE_C_FLAGS_RELEASE=-Os -DNDEBUG"
        "-DCMAKE_CXX_FLAGS_RELEASE=-Os -DNDEBUG")
endif()

# Sub-builds inherit the generator, so pass this only when multi-config -
# where CMAKE_BUILD_TYPE above is ignored - not into every single-config cache.
if(CMAKE_CONFIGURATION_TYPES)
    list(APPEND EP_CMAKE_ARGS "-DCMAKE_CONFIGURATION_TYPES=Release")
endif()

# Only when nasm is pinned: pre-filling this makes libjpeg-turbo's own
# search find it already answered.
if(IMAGEDECODER_PIN_NASM)
    list(APPEND EP_CMAKE_ARGS "-DCMAKE_ASM_NASM_COMPILER=${Nasm_EXECUTABLE}")
endif()

if(ANDROID)
    if(ANDROID_ABI STREQUAL "arm64-v8a")
        set(ANDROID_TARGET "aarch64-linux-android")
        set(ANDROID_CPU_FAMILY "aarch64")
        set(ANDROID_CPU "aarch64")
    elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
        set(ANDROID_TARGET "armv7a-linux-androideabi")
        set(ANDROID_CPU_FAMILY "arm")
        set(ANDROID_CPU "armv7-a")
    elseif(ANDROID_ABI STREQUAL "x86")
        set(ANDROID_TARGET "i686-linux-android")
        set(ANDROID_CPU_FAMILY "x86")
        set(ANDROID_CPU "i686")
    elseif(ANDROID_ABI STREQUAL "x86_64")
        set(ANDROID_TARGET "x86_64-linux-android")
        set(ANDROID_CPU_FAMILY "x86_64")
        set(ANDROID_CPU "x86_64")
    else()
        message(FATAL_ERROR "Unsupported architecture: ${ANDROID_ABI}")
    endif()

    list(APPEND EP_CMAKE_ARGS
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DANDROID_ABI=${ANDROID_ABI}"
        "-DANDROID_PLATFORM=${ANDROID_PLATFORM}"
        "-DCMAKE_SYSROOT=${CMAKE_SYSROOT}"
        "-DCMAKE_FIND_LIBRARY_SUFFIXES=${CMAKE_FIND_LIBRARY_SUFFIXES_STR}"
    )


    # meson has no toolchain-file equivalent, so describe the cross build
    # separately.
    set(MESON_CROSS_FILE "${CMAKE_BINARY_DIR}/android-cross.ini")
    file(WRITE ${MESON_CROSS_FILE} "
[host_machine]
system = 'android'
cpu_family = '${ANDROID_CPU_FAMILY}'
cpu = '${ANDROID_CPU}'
endian = 'little'

[binaries]
c = ['${CMAKE_C_COMPILER}', '--target=${ANDROID_TARGET}${ANDROID_PLATFORM_LEVEL}']
cpp = ['${CMAKE_CXX_COMPILER}', '--target=${ANDROID_TARGET}${ANDROID_PLATFORM_LEVEL}']
ar = '${CMAKE_AR}'
strip = '${CMAKE_STRIP}'
ranlib = '${CMAKE_RANLIB}'
as = '${CMAKE_ASM_COMPILER}'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-I${THIRD_PARTY_LIB_PATH}/include']
cpp_args = ['-I${THIRD_PARTY_LIB_PATH}/include']
c_link_args = ['-L${THIRD_PARTY_LIB_PATH}/lib']
cpp_link_args = ['-L${THIRD_PARTY_LIB_PATH}/lib']
pkg_config_path = ['${THIRD_PARTY_LIB_PATH}/lib/pkgconfig']

[properties]
pkg_config_libdir = '${THIRD_PARTY_LIB_PATH}/lib/pkgconfig'
")

endif()

if(EMSCRIPTEN)
    list(APPEND EP_CMAKE_ARGS
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DCMAKE_CROSSCOMPILING_EMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
    )
    # Native SIMD and CPU detection are x86 code here; WASM SIMD instead.
    set(IMAGEDECODER_NO_NATIVE_SIMD ON)

    get_filename_component(_em_dir "${CMAKE_C_COMPILER}" DIRECTORY)
    if(IMAGEDECODER_WASM_THREADS)
        set(_em_args "'-pthread', '-msimd128'")
        set(_em_link "'-pthread'")
    else()
        set(_em_args "'-msimd128'")
        set(_em_link "")
    endif()
    set(MESON_CROSS_FILE "${CMAKE_BINARY_DIR}/emscripten-cross.ini")
    file(WRITE ${MESON_CROSS_FILE} "
[host_machine]
system = 'emscripten'
cpu_family = 'wasm32'
cpu = 'wasm32'
endian = 'little'

[binaries]
c = '${_em_dir}/emcc'
cpp = '${_em_dir}/em++'
ar = '${_em_dir}/emar'
ranlib = '${_em_dir}/emranlib'
pkg-config = 'pkg-config'

[built-in options]
c_args = [${_em_args}]
cpp_args = [${_em_args}]
c_link_args = [${_em_link}]
cpp_link_args = [${_em_link}]
")
endif()

# dav1d: the one dependency that isn't a CMake project.
set(EP_MESON_ARGS
    "--prefix=${THIRD_PARTY_LIB_PATH}"
    "--libdir=lib"
    "--default-library=static"
    "--buildtype=release"
)

if(ANDROID OR EMSCRIPTEN)
    list(APPEND EP_MESON_ARGS "--cross-file=${MESON_CROSS_FILE}")
elseif(Nasm_EXECUTABLE)
    # dav1d finds its assembler via meson's find_program (PATH) - same
    # wrong-nasm risk as above. [binaries] is the only override from here.
    set(MESON_NATIVE_FILE "${CMAKE_BINARY_DIR}/native.ini")
    file(WRITE ${MESON_NATIVE_FILE}
         "[binaries]\nnasm = '${Nasm_EXECUTABLE}'\n")
    list(APPEND EP_MESON_ARGS "--native-file=${MESON_NATIVE_FILE}")
endif()

# Every dependency below is pinned, so the update step has nothing to do -
# but ExternalProject runs it every build anyway, invalidating
# patch/configure/build and rebuilding all sixteen for nothing. Download
# still re-runs when a GIT_TAG changes, since its stamp tracks repo info.
set_property(DIRECTORY PROPERTY EP_UPDATE_DISCONNECTED 1)

if(IMAGEDECODER_BUILD_DEPS)
    if(IMAGEDECODER_PIN_NASM)
        include("cmake/nasm.cmake")
    endif()

    include("cmake/zlib.cmake")
    include("cmake/lcms2.cmake")

    include("cmake/libjpegturbo.cmake")
    include("cmake/libtiff.cmake")
    include("cmake/libpng.cmake")
    include("cmake/libopenjp2.cmake")
    include("cmake/libwebp.cmake")
    include("cmake/giflib.cmake")

    include("cmake/dav1d.cmake")
    include("cmake/libde265.cmake")
    include("cmake/libheif.cmake")
    include("cmake/libavif.cmake")
    include("cmake/libultrahdr.cmake")

    include("cmake/brotli.cmake")
    include("cmake/highway.cmake")
    include("cmake/libjxl.cmake")

    # The two that assemble x86 SIMD; ordered after ep_nasm rather than
    # relying on PATH.
    if(IMAGEDECODER_PIN_NASM)
        add_dependencies(ep_libjpegturbo ep_nasm)
        add_dependencies(ep_dav1d ep_nasm)
    endif()

    # Read from the buildsystem rather than hand-listed, so a transitive
    # dependency (via libjxl/libheif) can't silently drop out of the tarball.
    get_property(IMAGEDECODER_DEP_TARGETS
                 DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    list(FILTER IMAGEDECODER_DEP_TARGETS INCLUDE REGEX "^ep_")
    list(LENGTH IMAGEDECODER_DEP_TARGETS _n_deps)
    if(_n_deps LESS 16)
        message(FATAL_ERROR
            "expected at least 16 dependency targets, found ${_n_deps}: "
            "${IMAGEDECODER_DEP_TARGETS}")
    endif()

    # The third-party tree alone, matching tools/make-prebuilt.sh - not the
    # default target, which would also build our own library and CLI.
    add_custom_target(imagedecoder_deps DEPENDS ${IMAGEDECODER_DEP_TARGETS})
else()
    # Copied into the fakeroot rather than used in place, so nothing writes
    # back into a shared/read-only cache.
    if(NOT EXISTS "${IMAGEDECODER_PREBUILT_DEPS}/lib/pkgconfig")
        message(FATAL_ERROR
            "IMAGEDECODER_PREBUILT_DEPS has no lib/pkgconfig: "
            "${IMAGEDECODER_PREBUILT_DEPS}")
    endif()

    # A tarball built from different pins would link but with the wrong
    # decoders - the one failure mode here that doesn't announce itself.
    include("${PROJECT_SOURCE_DIR}/tools/dep-pins.cmake")
    imagedecoder_dep_pins(_want_pins "${PROJECT_SOURCE_DIR}/cmake")
    set(_manifest "${IMAGEDECODER_PREBUILT_DEPS}/MANIFEST.txt")
    if(NOT EXISTS "${_manifest}")
        message(FATAL_ERROR
            "${IMAGEDECODER_PREBUILT_DEPS} has no MANIFEST.txt, so the "
            "dependency versions in it cannot be checked against this "
            "checkout. Unpack a tarball from tools/make-prebuilt.sh.")
    endif()
    file(STRINGS "${_manifest}" _pin_line REGEX "^pins:")
    string(REGEX REPLACE "^pins:[ \t]*" "" _have_pins "${_pin_line}")
    if(NOT _have_pins STREQUAL _want_pins)
        message(FATAL_ERROR
            "prebuilt dependencies are for a different set of pins.\n"
            "  tree:     ${IMAGEDECODER_PREBUILT_DEPS}\n"
            "  built as: ${_have_pins}\n"
            "  wanted:   ${_want_pins}\n"
            "Unpack the tarball matching this checkout, or leave "
            "IMAGEDECODER_PREBUILT_DEPS unset to build from source.")
    endif()

    message(STATUS "imagedecoder: prebuilt dependencies ${_want_pins} from "
                   "${IMAGEDECODER_PREBUILT_DEPS}")
    file(COPY "${IMAGEDECODER_PREBUILT_DEPS}/lib"
              "${IMAGEDECODER_PREBUILT_DEPS}/include"
         DESTINATION "${THIRD_PARTY_LIB_PATH}")

    set(IMAGEDECODER_DEP_TARGETS "")
endif()

include(ExternalProject)

# Windows ships no pkg-config and src/ and cli/ both want one, prebuilt tree
# or not - hence outside the branch above. Out of imagedecoder_deps too: a
# host tool is not something make-prebuilt.sh should carry.
if(WIN32)
    include("cmake/pkgconf.cmake")
    list(APPEND IMAGEDECODER_DEP_TARGETS ep_pkgconf)
    set(IMAGEDECODER_PKG_CONFIG_ARG
        "-DPKG_CONFIG_EXECUTABLE=${THIRD_PARTY_LIB_PATH}/bin/pkg-config.exe")
endif()

# The portable core: installs libimagedecoder.a and headers for the JNI
# layer below (or any other consumer).
ExternalProject_Add(ep_imagedecoder
    SOURCE_DIR ${PROJECT_SOURCE_DIR}/src
    DEPENDS ${IMAGEDECODER_DEP_TARGETS}
    BUILD_ALWAYS 1
    CMAKE_ARGS
        ${EP_CMAKE_ARGS}
        "-DIMAGEDECODER_PREFIX=${THIRD_PARTY_LIB_PATH}"
        "-DIMAGEDECODER_WASM_THREADS=${IMAGEDECODER_WASM_THREADS}"
        ${IMAGEDECODER_PKG_CONFIG_ARG}
)

# CLI front end for exercising decoders without an Android device or
# gradle. Host only - Android reaches the library via JNI, the web via wasm/.
if(NOT ANDROID AND NOT EMSCRIPTEN)
    ExternalProject_Add(ep_imagedecoder_cli
        SOURCE_DIR ${PROJECT_SOURCE_DIR}/cli
        DEPENDS ep_imagedecoder
        BUILD_ALWAYS 1
        CMAKE_ARGS
            ${EP_CMAKE_ARGS}
            "-DIMAGEDECODER_PREFIX=${THIRD_PARTY_LIB_PATH}"
            "-DIMAGEDECODER_CMAKE_DIR=${PROJECT_SOURCE_DIR}/cmake"
            ${IMAGEDECODER_PKG_CONFIG_ARG}
    )
endif()

if(ANDROID)
    # ExternalProject rather than add_subdirectory so it configures only
    # after the fakeroot exists.
    ExternalProject_Add(ep_imagedecoder_jni
        SOURCE_DIR ${PROJECT_SOURCE_DIR}/android/jni
        DEPENDS ep_imagedecoder
        INSTALL_COMMAND ""
        BUILD_ALWAYS 1
        CMAKE_ARGS
            ${EP_CMAKE_ARGS}
            "-DIMAGEDECODER_PREFIX=${THIRD_PARTY_LIB_PATH}"
            "-DIMAGEDECODER_CMAKE_DIR=${PROJECT_SOURCE_DIR}/cmake"
            "-DCMAKE_LIBRARY_OUTPUT_DIRECTORY=${CMAKE_LIBRARY_OUTPUT_DIRECTORY}"
            "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    )
endif()

if(EMSCRIPTEN)
    # The JS module: imagedecoder.js and imagedecoder.wasm in fakeroot/wasm.
    ExternalProject_Add(ep_imagedecoder_wasm
        SOURCE_DIR ${PROJECT_SOURCE_DIR}/wasm
        DEPENDS ep_imagedecoder
        BUILD_ALWAYS 1
        CMAKE_ARGS
            ${EP_CMAKE_ARGS}
            "-DCMAKE_INSTALL_PREFIX=${THIRD_PARTY_LIB_PATH}"
            "-DIMAGEDECODER_PREFIX=${THIRD_PARTY_LIB_PATH}"
            "-DIMAGEDECODER_WASM_THREADS=${IMAGEDECODER_WASM_THREADS}"
            "-DIMAGEDECODER_CMAKE_DIR=${PROJECT_SOURCE_DIR}/cmake"
    )
endif()
