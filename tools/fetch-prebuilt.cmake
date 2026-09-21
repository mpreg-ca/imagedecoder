# Downloads the prebuilt dependency tarball for this checkout's pins and
# unpacks it into the build directory, skipping ~25 min of compiling. Included
# by the superbuild via IMAGEDECODER_FETCH_PREBUILT_DEPS; not meant to run standalone.
#
# Matched by pin hash, not "latest" - a mismatched release would silently ship
# the wrong decoders. No matching release (e.g. pins just changed) is not an
# error; it just falls back to a source build.

include("${CMAKE_CURRENT_LIST_DIR}/dep-pins.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/platform.cmake")

# Sets out_var to the unpacked directory, or leaves it empty.
function(imagedecoder_fetch_prebuilt out_var)
    set(${out_var} "" PARENT_SCOPE)

    imagedecoder_dep_pins(_pins "${PROJECT_SOURCE_DIR}/cmake")
    imagedecoder_platform_triple(_triple)

    set(_name "imagedecoder-deps-${_triple}-${_pins}")
    set(_out "${CMAKE_BINARY_DIR}/prebuilt")
    set(_dir "${_out}/${_name}")

    # Already unpacked by an earlier configure.
    if(EXISTS "${_dir}/MANIFEST.txt")
        set(${out_var} "${_dir}" PARENT_SCOPE)
        return()
    endif()

    set(_url
        "${IMAGEDECODER_PREBUILT_URL}/deps-${_pins}/${_name}.tar.gz")
    set(_tmp "${_out}/.download")
    file(REMOVE_RECURSE "${_tmp}")

    # Digest first: a truncated or substituted archive then fails before unpacking.
    file(DOWNLOAD "${_url}.sha256" "${_tmp}/sha" STATUS _st
         TIMEOUT 60 INACTIVITY_TIMEOUT 30)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
        list(GET _st 1 _why)
        message(STATUS
            "imagedecoder: no published prebuilt for ${_name} (${_why})")
        file(REMOVE_RECURSE "${_tmp}")
        return()
    endif()

    # + and a length check, not [0-9a-f]{64}: CMake's regex has no brace quantifier.
    file(READ "${_tmp}/sha" _sha_text)
    string(STRIP "${_sha_text}" _sha_text)
    if(NOT _sha_text MATCHES "^([0-9a-fA-F]+)")
        message(FATAL_ERROR "${_name}.tar.gz.sha256 carries no digest")
    endif()
    set(_sha "${CMAKE_MATCH_1}")
    string(LENGTH "${_sha}" _len)
    if(NOT _len EQUAL 64)
        message(FATAL_ERROR
            "${_name}.tar.gz.sha256 holds a ${_len}-character digest, "
            "not a sha256")
    endif()

    message(STATUS "imagedecoder: fetching ${_name}")
    # EXPECTED_HASH fails hard on a bad archive instead of silently falling
    # back to source, so the status check below is only ever a transport failure.
    file(DOWNLOAD "${_url}" "${_tmp}/t.tar.gz"
         EXPECTED_HASH "SHA256=${_sha}"
         STATUS _st TIMEOUT 900 INACTIVITY_TIMEOUT 60)
    list(GET _st 0 _code)
    if(NOT _code EQUAL 0)
        list(GET _st 1 _why)
        file(REMOVE_RECURSE "${_tmp}")
        message(STATUS "imagedecoder: could not fetch ${_name} (${_why})")
        return()
    endif()

    file(ARCHIVE_EXTRACT INPUT "${_tmp}/t.tar.gz" DESTINATION "${_out}")
    file(REMOVE_RECURSE "${_tmp}")
    if(NOT EXISTS "${_dir}/MANIFEST.txt")
        message(FATAL_ERROR "${_name}.tar.gz did not unpack to ${_dir}")
    endif()

    set(${out_var} "${_dir}" PARENT_SCOPE)
endfunction()
