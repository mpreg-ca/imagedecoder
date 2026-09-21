# The platform part of a prebuilt tarball's name: <os>-<arch>-<libc>.
#
# One implementation shared by tools/make-prebuilt.sh (names what it packs)
# and tools/fetch-prebuilt.cmake (asks for what it wants) - if they spelled a
# platform differently, fetch would never find its own tarball.
#
# Included by the superbuild, where CMake already knows the (possibly cross)
# target; run standalone via `cmake -P` by make-prebuilt.sh, which falls back
# to uname and PREBUILT_OS/ARCH/LIBC for a cross build uname can't describe.

function(imagedecoder_platform_triple out_var)
    # Android is always a cross build, and its own ABI names are the arch.
    if(ANDROID)
        set(${out_var} "android-${ANDROID_ABI}-bionic" PARENT_SCOPE)
        return()
    endif()

    # The toolchain reports an x86 processor; the objects are wasm32 (threads or
    # not, SIMD128, wasm exceptions) and only link with the Emscripten that made
    # them, built the same way.
    if(EMSCRIPTEN)
        set(_st "")
        if(DEFINED IMAGEDECODER_WASM_THREADS AND NOT IMAGEDECODER_WASM_THREADS)
            set(_st "-nothreads")
        endif()
        set(${out_var} "emscripten-wasm32-em${EMSCRIPTEN_VERSION}${_st}" PARENT_SCOPE)
        return()
    endif()

    set(_os "$ENV{PREBUILT_OS}")
    set(_arch "$ENV{PREBUILT_ARCH}")
    set(_libc "$ENV{PREBUILT_LIBC}")

    if(NOT _os)
        if(WIN32)
            set(_os windows)
        elseif(APPLE)
            set(_os darwin)
        elseif(CMAKE_SYSTEM_NAME)
            string(TOLOWER "${CMAKE_SYSTEM_NAME}" _os)
        else()
            execute_process(COMMAND uname -s OUTPUT_VARIABLE _os
                            ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
            string(TOLOWER "${_os}" _os)
        endif()
        # git-for-Windows bash's uname answers with the emulation layer/kernel
        # build (mingw64_nt-10.0-26100), which varies across runner images.
        if(_os MATCHES "^(mingw|msys|cygwin)")
            set(_os windows)
        endif()
    endif()

    if(NOT _arch)
        if(CMAKE_SYSTEM_PROCESSOR)
            set(_arch "${CMAKE_SYSTEM_PROCESSOR}")
        else()
            execute_process(COMMAND uname -m OUTPUT_VARIABLE _arch
                            ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
        endif()
        # Normalize to what uname -m would say, since tarballs are named that way.
        if(_arch STREQUAL "AMD64")
            set(_arch x86_64)
        endif()
    endif()

    if(NOT _libc)
        if(_os STREQUAL "windows")
            set(_libc msvc)
        elseif(_os STREQUAL "darwin")
            set(_libc libc++)
        else()
            # musl and glibc archives aren't interchangeable; the compiler
            # knows which it targets, else fall back to ldd.
            set(_libc glibc)
            if(CMAKE_C_COMPILER)
                execute_process(COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
                                OUTPUT_VARIABLE _probe ERROR_QUIET
                                OUTPUT_STRIP_TRAILING_WHITESPACE)
            else()
                execute_process(COMMAND ldd --version
                                OUTPUT_VARIABLE _probe ERROR_VARIABLE _probe
                                OUTPUT_STRIP_TRAILING_WHITESPACE)
            endif()
            if(_probe MATCHES "musl")
                set(_libc musl)
            endif()
        endif()
    endif()

    set(${out_var} "${_os}-${_arch}-${_libc}" PARENT_SCOPE)
endfunction()

# Print under `cmake -P`; compared against this file so including it from
# another -P script prints nothing.
if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    imagedecoder_platform_triple(_t)
    execute_process(COMMAND ${CMAKE_COMMAND} -E echo "${_t}")
endif()
