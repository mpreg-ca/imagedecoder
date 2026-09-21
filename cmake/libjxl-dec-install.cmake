# Installs libjxl's decoder-only library and the thread runner by hand:
# libjxl's install target would build the encoder as well.
#   -DBIN=<libjxl build dir> -DPREFIX=<install prefix>

file(GLOB_RECURSE _libs
     "${BIN}/lib/*jxl_dec.a" "${BIN}/lib/*jxl_dec.lib"
     "${BIN}/lib/*jxl_threads.a" "${BIN}/lib/*jxl_threads.lib")
list(LENGTH _libs _n)
if(NOT _n EQUAL 2)
    message(FATAL_ERROR "expected jxl_dec and jxl_threads in ${BIN}/lib, found: ${_libs}")
endif()
file(INSTALL ${_libs} DESTINATION "${PREFIX}/lib")

# Configured into the build tree with the generated version.h and export headers.
file(GLOB _headers "${BIN}/lib/include/jxl/*.h")
list(FILTER _headers EXCLUDE REGEX "/(encode|encode_cxx|stats)\\.h$")
file(INSTALL ${_headers} DESTINATION "${PREFIX}/include/jxl")

file(INSTALL "${BIN}/lib/libjxl_threads.pc" DESTINATION "${PREFIX}/lib/pkgconfig")

# libjxl.pc, pointed at the decoder: no encoder, no brotlienc, no CMS.
file(READ "${BIN}/lib/libjxl.pc" _pc)
string(REPLACE "Name: libjxl" "Name: libjxl_dec" _pc "${_pc}")
string(REPLACE "Loads and saves JPEG XL files" "Loads JPEG XL files" _pc "${_pc}")
string(REGEX REPLACE "Requires: [^\n]*" "Requires: libhwy libbrotlidec" _pc "${_pc}")
string(REGEX REPLACE "-ljxl(-static)?( |\n)" "-ljxl_dec\\2" _pc "${_pc}")
file(WRITE "${PREFIX}/lib/pkgconfig/libjxl_dec.pc" "${_pc}")
