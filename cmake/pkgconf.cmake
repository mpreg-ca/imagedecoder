include(ExternalProject)

set(PKGCONF_VERSION 3.0.7)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "ARM64|arm64|aarch64")
    set(PKGCONF_ARCH arm64)
    set(PKGCONF_SHA256
        bc690b61cf20d0f8fa81b263482c1c50afe852298301bb7f99bbead80c19f7dc)
else()
    set(PKGCONF_ARCH x64)
    set(PKGCONF_SHA256
        7a316dba4a4498ea952b746c82deed41c597657095a0f776b75654191b45ae44)
endif()
message(STATUS "imagedecoder: pkgconf ${PKGCONF_VERSION} (pinned, ${PKGCONF_ARCH})")

# Only .msi assets are published; the install script unpacks it.
ExternalProject_Add(ep_pkgconf
    URL "https://github.com/pkgconf/pkgconf/releases/download/pkgconf-${PKGCONF_VERSION}/pkgconf-${PKGCONF_ARCH}-${PKGCONF_VERSION}.msi"
    URL_HASH SHA256=${PKGCONF_SHA256}
    DOWNLOAD_NO_EXTRACT TRUE
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    INSTALL_COMMAND
        ${CMAKE_COMMAND}
            "-DMSI=<DOWNLOADED_FILE>"
            "-DBINDIR=${THIRD_PARTY_LIB_PATH}/bin"
            -P "${PROJECT_SOURCE_DIR}/cmake/pkgconf-install.cmake"
)
