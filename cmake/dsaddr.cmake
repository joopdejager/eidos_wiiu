#
# cmake/dsaddr.cmake -- route the matched sources through tools/dsaddr.py.
#
# docs/boot.md, "Fixed addresses": a matched file's casts of DS addresses
# (`*(vu16 *)0x04000208`) are rewritten in a copy in the build tree into
# calls of sdk/dsmem.h's bw_ds_ptr, and the copy is what gets compiled.  The
# matched file itself is never touched (portability rule 1).
#
# Every source goes through the rewrite, including the ones it leaves alone
# (those come out as a byte-identical copy), so there is one path per file
# and no configure-time guess that an edit could make stale.  The copy
# depends on its input and on the tool, so editing either reruns the
# rewrite and recompiles; it is an ordinary custom command, which every
# generator (Ninja, Makefiles, Visual Studio) supports.
#
# Two things change with the path and are carried over here:
#   - quoted #includes are looked up next to the including file first, so
#     the original file's directory is added to the copy's include path
#     (a few matched files say "../arm9_sdk/libc/string.h");
#   - per-source properties must be set on the copy, not the original:
#     use eidos_dsaddr_path() to name it.
#
# Diagnostics still name the original file and line: a rewritten copy
# starts with `#line 1 "<original>"` and keeps every line where it was.
#

set(EIDOS_DSADDR_TOOL ${CMAKE_CURRENT_LIST_DIR}/../tools/dsaddr.py)
get_filename_component(EIDOS_DSADDR_TOOL "${EIDOS_DSADDR_TOOL}" ABSOLUTE)
set(EIDOS_DSADDR_DIR ${CMAKE_BINARY_DIR}/dsaddr)
# A big-endian target keeps the register file word-swizzled (sdk/dsmem.c,
# io_host), so each rewritten access also names its width.
set(EIDOS_DSADDR_FLAGS)
if(CMAKE_C_BYTE_ORDER STREQUAL "BIG_ENDIAN")
    set(EIDOS_DSADDR_FLAGS --width)
    option(EIDOS_SSO "Store the types in tools/sso_types.txt in the DS byte and bitfield order" OFF)
    if(EIDOS_SSO)
        list(APPEND EIDOS_DSADDR_FLAGS --sso)
        get_filename_component(EIDOS_SSO_TYPES "${CMAKE_CURRENT_LIST_DIR}/../tools/sso_types.txt" ABSOLUTE)
        add_compile_definitions(EIDOS_SSO=1)
    endif()
endif()
get_filename_component(EIDOS_GAMECUT_TOOL "${CMAKE_CURRENT_LIST_DIR}/../tools/gamecut.py" ABSOLUTE)
# The final returns the copies get (tools/gamereturns.txt, gamecut add_returns).
get_filename_component(EIDOS_GAMERETURNS "${CMAKE_CURRENT_LIST_DIR}/../tools/gamereturns.txt" ABSOLUTE)

# eidos_dsaddr_path(<var> <source>): the build-tree copy of <source>.
function(eidos_dsaddr_path var src)
    get_filename_component(src "${src}" ABSOLUTE)
    file(RELATIVE_PATH rel "${ALETHEIA_ROOT}/src/matched" "${src}")
    if(rel MATCHES "^\.\./")
        # A src/game file (docs/game-tree.md): under game/.
        file(RELATIVE_PATH grel "${ALETHEIA_ROOT}/src/game" "${src}")
        if(NOT grel MATCHES "^\.\./" AND NOT IS_ABSOLUTE "${grel}")
            set(rel "game/${grel}")
        endif()
    endif()
    if(rel MATCHES "^\\.\\./" OR IS_ABSOLUTE "${rel}")
        # Not under src/matched: keep the name, keyed by the full path so
        # two files with one name cannot collide.
        string(MD5 key "${src}")
        string(SUBSTRING "${key}" 0 12 key)
        get_filename_component(name "${src}" NAME)
        set(rel "_other/${key}/${name}")
    endif()
    set(${var} "${EIDOS_DSADDR_DIR}/${rel}" PARENT_SCOPE)
endfunction()

# eidos_dsaddr(<out_list_var> <sources>...): add the rewrite of each source
# and return the copies, in order.
function(eidos_dsaddr out_var)
    set(outs "")
    foreach(src IN LISTS ARGN)
        get_filename_component(src "${src}" ABSOLUTE)
        eidos_dsaddr_path(out "${src}")
        get_filename_component(src_dir "${src}" DIRECTORY)
        file(RELATIVE_PATH shown "${ALETHEIA_ROOT}" "${src}")
        # A src/game unit: its copy also has the port's own functions and
        # its asm bodies cut out (tools/gamecut.py, docs/game-tree.md).
        set(cut_args)
        set(cut_deps)
        if(shown MATCHES "^src/game/")
            list(APPEND cut_args --cut-asm)
            list(APPEND cut_deps ${EIDOS_GAMECUT_TOOL} ${EIDOS_GAMERETURNS})
            if(EIDOS_GAME_CUT_LIST)
                list(APPEND cut_args --cut-list ${EIDOS_GAME_CUT_LIST})
                list(APPEND cut_deps ${EIDOS_GAME_CUT_LIST})
            endif()
        endif()
        add_custom_command(
            OUTPUT ${out}
            COMMAND ${Python3_EXECUTABLE} ${EIDOS_DSADDR_TOOL}
                    ${src} -o ${out} --name ${src} ${EIDOS_DSADDR_FLAGS} ${cut_args}
            DEPENDS ${src} ${EIDOS_DSADDR_TOOL} ${EIDOS_SSO_TYPES} ${cut_deps}
            COMMENT "dsaddr ${shown}"
            VERBATIM)
        set_source_files_properties(${out} PROPERTIES
            GENERATED TRUE
            INCLUDE_DIRECTORIES "${src_dir}")
        list(APPEND outs ${out})
    endforeach()
    set(${out_var} ${outs} PARENT_SCOPE)
endfunction()

# The matched sources come out of a checkout aletheia is still editing, so
# the port's build is pinned to one revision of it.  Report at configure time
# when the checkout has moved off that revision (docs/aletheia-pin.md); a warning,
# never an error.  dsaddr.cmake is included unconditionally, which is why the
# check hangs off it rather than off a target.
include(${CMAKE_CURRENT_LIST_DIR}/aletheia_pin.cmake)
