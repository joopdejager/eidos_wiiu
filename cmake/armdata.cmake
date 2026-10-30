#
# cmake/armdata.cmake -- generate the ARM9 data tables and the forced-include
# header for a set of matched sources (tools/armdata.py, docs/armdata.md).
#
#   eidos_armdata(<prefix>
#       SOURCES_LIST <file>       the build's sources, one path per line
#       [REGION arm9]             or ovNNN (compiled overlays use cmake/overlays.cmake)
#       [MATCHED <src>...]        the matched sources to compile with the
#                                 header force-included
#       [BIND <src>...]           port sources whose data names are bound
#                                 like a matched file's; the caller includes
#                                 the header in them itself
#       [CODE_SCAN])              list every function the sources define for
#                                 the run-time scan for code pointers the
#                                 metadata misses (docs/armdata.md); only for
#                                 a program that links them all (the boot)
#
# Writes <build>/armdata/<prefix>.c and <prefix>.h and sets <prefix>_C and
# <prefix>_H in the caller's scope.  The .c goes into the target that links
# the matched sources; the .h is force-included into every one of them
# (`-include` / `/FI`), and each of them depends on it, so it is generated
# before they compile and a change to it recompiles them.
#
# The list file is usually written with file(GENERATE) from the targets'
# SOURCES.  A dsaddr copy in it stands for its original in aletheia's
# src/matched/ (the generator maps it back, so it need not exist yet);
# another source the build generates is skipped until it exists, and the
# generator's own outputs are ignored.
#
# Nothing here reads a game byte: the inputs are aletheia's metadata.
#

set(EIDOS_ARMDATA_TOOL ${CMAKE_CURRENT_LIST_DIR}/../tools/armdata.py)
get_filename_component(EIDOS_ARMDATA_TOOL "${EIDOS_ARMDATA_TOOL}" ABSOLUTE)
set(EIDOS_ARMDATA_EXCLUSIONS ${CMAKE_CURRENT_LIST_DIR}/../tools/armdata_exclusions.txt)
get_filename_component(EIDOS_ARMDATA_EXCLUSIONS "${EIDOS_ARMDATA_EXCLUSIONS}" ABSOLUTE)
get_filename_component(EIDOS_ARMDATA_WIDTHS "${CMAKE_CURRENT_LIST_DIR}/../tools/armdata_widths.txt" ABSOLUTE)
# The struct layouts of the sources' declarations (armdata.py imports it).
get_filename_component(EIDOS_DATALAYOUT_TOOL "${CMAKE_CURRENT_LIST_DIR}/../tools/datalayout.py" ABSOLUTE)

function(eidos_armdata prefix)
    cmake_parse_arguments(A "CODE_SCAN" "SOURCES_LIST;REGION" "MATCHED;BIND" ${ARGN})
    if(NOT A_SOURCES_LIST)
        message(FATAL_ERROR "eidos_armdata(${prefix}): SOURCES_LIST is required")
    endif()
    if(NOT A_REGION)
        set(A_REGION arm9)
    endif()
    set(dir ${CMAKE_BINARY_DIR}/armdata)
    file(MAKE_DIRECTORY ${dir})
    set(out_c ${dir}/${prefix}.c)
    set(out_h ${dir}/${prefix}.h)

    # The listings are generated in aletheia and may be absent; the
    # generator falls back to data_objects.json alone.
    if(A_REGION STREQUAL "arm9")
        file(GLOB listings ${ALETHEIA_ROOT}/asm/arm9/*.s)
    else()
        file(GLOB listings ${ALETHEIA_ROOT}/asm/overlays/${A_REGION}.s)
    endif()

    set(bind_args)
    if(A_CODE_SCAN)
        list(APPEND bind_args --code-scan)
    endif()
    foreach(src ${A_BIND})
        list(APPEND bind_args --bind-source ${src})
    endforeach()
    # The player's ARM9 image beside the data root: the veneers and adjustor
    # thunks the data points at, which the listing would name (optional).
    get_filename_component(_armdata_extract_dir "${BW_DATA_ROOT_ABS}" DIRECTORY)
    if(A_REGION STREQUAL "arm9" AND EXISTS "${_armdata_extract_dir}/arm9_dec.bin")
        list(APPEND bind_args --arm9-image ${_armdata_extract_dir}/arm9_dec.bin)
    endif()

    # src/game: read each unit as its cut copy compiles (tools/gamecut.py).
    set(_armdata_cut_dep)
    if(EIDOS_SOURCE_TREE STREQUAL "game" AND EIDOS_GAME_CUT_LIST)
        list(APPEND bind_args --cut-list ${EIDOS_GAME_CUT_LIST})
        set(_armdata_cut_dep ${EIDOS_GAME_CUT_LIST})
    endif()

    add_custom_command(
        OUTPUT ${out_c} ${out_h}
        COMMAND ${Python3_EXECUTABLE} ${EIDOS_ARMDATA_TOOL}
                --aletheia-root ${ALETHEIA_ROOT}
                --sources ${A_SOURCES_LIST}
                --region ${A_REGION}
                ${bind_args}
                --exclusions ${EIDOS_ARMDATA_EXCLUSIONS}
                --out-c ${out_c} --out-h ${out_h}
        DEPENDS ${EIDOS_ARMDATA_TOOL} ${EIDOS_ARMDATA_WIDTHS} ${EIDOS_DATALAYOUT_TOOL}
                ${EIDOS_ARMDATA_EXCLUSIONS}
                ${A_SOURCES_LIST}
                ${A_BIND}
                ${_armdata_cut_dep}
                ${ALETHEIA_ROOT}/layout/layout.json
                ${ALETHEIA_ROOT}/layout/data_objects.json
                ${ALETHEIA_ROOT}/layout/function_addrs.tsv
                ${ALETHEIA_ROOT}/layout/symbols_sdk.txt
                ${listings}
        COMMENT "Generating ${prefix} (ARM9 data names, relocations) from ${ALETHEIA_ROOT}/layout"
        VERBATIM)
    set_source_files_properties(${out_c} ${out_h} PROPERTIES GENERATED TRUE)

    if(A_MATCHED)
        if(MSVC)
            set(opt "/FI${out_h}")
        else()
            set(opt "-include;${out_h}")
        endif()
        set_property(SOURCE ${A_MATCHED} APPEND PROPERTY COMPILE_OPTIONS ${opt})
        set_property(SOURCE ${A_MATCHED} APPEND PROPERTY OBJECT_DEPENDS ${out_h})
    endif()

    set(${prefix}_C ${out_c} PARENT_SCOPE)
    set(${prefix}_H ${out_h} PARENT_SCOPE)
endfunction()
