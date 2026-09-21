# Lifts pkg-config.exe out of the pkgconf .msi. msiexec /a unpacks without
# elevation, in the MSI's own layout - hence the search - and can return before
# the writes land - hence the wait.

# msiexec rejects a TARGETDIR holding a ".." segment.
get_filename_component(_parent "${BINDIR}" DIRECTORY)
set(_stage "${_parent}/pkgconf-msi")
file(REMOVE_RECURSE "${_stage}")
file(MAKE_DIRECTORY "${_stage}")

file(TO_NATIVE_PATH "${MSI}" _msi_native)
file(TO_NATIVE_PATH "${_stage}" _stage_native)

execute_process(
    COMMAND msiexec /a "${_msi_native}" /qn "TARGETDIR=${_stage_native}"
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "msiexec could not unpack ${MSI}: ${_rc}")
endif()

set(_exe "")
foreach(_try RANGE 60)
    file(GLOB_RECURSE _found "${_stage}/pkg-config.exe")
    if(_found)
        list(GET _found 0 _exe)
        break()
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1)
endforeach()

if(NOT _exe)
    message(FATAL_ERROR "no pkg-config.exe unpacked from ${MSI}")
endif()

file(MAKE_DIRECTORY "${BINDIR}")
file(COPY "${_exe}" DESTINATION "${BINDIR}")
file(REMOVE_RECURSE "${_stage}")
