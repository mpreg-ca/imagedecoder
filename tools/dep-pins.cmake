# A short hash over every pin in cmake/, used to name prebuilt tarballs so a
# consumer finds one built from exactly these pins or falls back to source.
#
# Lives in tools/, not cmake/, so it isn't itself part of what it hashes.
#
# Carriage returns stripped before hashing, or a Windows checkout would hash
# the same pins to a different value and never match a tarball built elsewhere.

function(imagedecoder_dep_pins out_var cmake_dir)
    file(GLOB _pin_files "${cmake_dir}/*.cmake")
    list(SORT _pin_files)

    set(_lines "")
    foreach(_f IN LISTS _pin_files)
        file(STRINGS "${_f}" _file_lines)
        foreach(_line IN LISTS _file_lines)
            if(_line MATCHES "GIT_TAG|GIT_REPOSITORY|NASM_VERSION|NASM_SHA256")
                string(REGEX REPLACE "\r" "" _line "${_line}")
                string(APPEND _lines "${_line}\n")
            endif()
        endforeach()
    endforeach()

    get_filename_component(_root "${cmake_dir}" DIRECTORY)
    file(STRINGS "${_root}/CMakeLists.txt" _root_lines
         REGEX "^set\\(IMAGEDECODER_DEPS_REVISION ")
    foreach(_line IN LISTS _root_lines)
        string(REGEX REPLACE "\r" "" _line "${_line}")
        string(APPEND _lines "${_line}\n")
    endforeach()

    string(SHA256 _full "${_lines}")
    string(SUBSTRING "${_full}" 0 12 _short)
    set(${out_var} "${_short}" PARENT_SCOPE)
endfunction()

# Print under `cmake -P`; compared against this file so including it from
# another -P script doesn't also print a stray hash.
if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    get_filename_component(_tools_dir "${CMAKE_SCRIPT_MODE_FILE}" DIRECTORY)
    get_filename_component(_repo_root "${_tools_dir}" DIRECTORY)
    imagedecoder_dep_pins(_pins "${_repo_root}/cmake")
    execute_process(COMMAND ${CMAKE_COMMAND} -E echo "${_pins}")
endif()
