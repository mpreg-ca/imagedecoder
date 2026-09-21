include(ExternalProject)

set(NASM_VERSION 2.16.03)
if(WIN32)
    set(NASM_PLATFORM win64)
    set(NASM_SHA256 3ee4782247bcb874378d02f7eab4e294a84d3d15f3f6ee2de2f47a46aa7226e6)
else()
    set(NASM_PLATFORM macosx)
    set(NASM_SHA256 0d29bcd8a5fc617333f4549c7c1f93d1866a4a0915c40359e0a8585bb1a5aa75)
endif()
message(STATUS "imagedecoder: nasm ${NASM_VERSION} (pinned, ${NASM_PLATFORM})")

ExternalProject_Add(ep_nasm
    URL "https://www.nasm.us/pub/nasm/releasebuilds/${NASM_VERSION}/${NASM_PLATFORM}/nasm-${NASM_VERSION}-${NASM_PLATFORM}.zip"
    URL_HASH SHA256=${NASM_SHA256}
    DOWNLOAD_EXTRACT_TIMESTAMP FALSE
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND
        ${CMAKE_COMMAND} -E make_directory ${THIRD_PARTY_LIB_PATH}/bin
    COMMAND
        ${CMAKE_COMMAND} -E copy_if_different
            <SOURCE_DIR>/${NASM_BINARY} ${Nasm_EXECUTABLE}
)
