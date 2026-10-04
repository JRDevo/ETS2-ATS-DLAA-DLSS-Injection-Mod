# Build-time check of the HLSL compute shaders embedded in the C++ sources (v0.5.6).
# Run as a script:  cmake -DFXC=<fxc.exe> -DSRC_DIR=<repo> -DOUT_DIR=<dir> -DBLOCKS=<list> -DSTAMP=<file> -P validate_shaders.cmake
#   BLOCKS = comma-separated "relative/source.cpp@kBlockName" entries.
#   v0.7.0: an entry may add "@profile@entry" (e.g. "src/scene_dlaa.cpp@kCompositeVs@vs_5_0@VSMain") for the
#   graphics shaders; without it the block is cs_5_0 / CSMain as before.
# Each block is the text between the marker comments
#   // kBlockName-BEGIN ...      and      // kBlockName-END
# and inside that, the C++ raw string  R"( ... )"  is the HLSL. It is written to OUT_DIR/kBlockName.hlsl and
# compiled with fxc (profile + entry as given, default cs_5_0 / CSMain, same as the runtime D3DCompile). Any
# failure stops the build.
foreach(_v FXC SRC_DIR OUT_DIR BLOCKS STAMP)
    if (NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "validate_shaders.cmake: ${_v} not set")
    endif()
endforeach()

file(MAKE_DIRECTORY "${OUT_DIR}")
string(REPLACE "," ";" _blocks "${BLOCKS}")
set(_n 0)
foreach(_entry IN LISTS _blocks)
    string(REPLACE "@" ";" _parts "${_entry}")
    list(GET _parts 0 _file)
    list(GET _parts 1 _name)
    list(LENGTH _parts _np)
    set(_profile cs_5_0)
    set(_entry CSMain)
    if (_np GREATER_EQUAL 4)                             # v0.7.0: "@profile@entry"
        list(GET _parts 2 _profile)
        list(GET _parts 3 _entry)
    endif()
    file(READ "${SRC_DIR}/${_file}" _src)

    string(FIND "${_src}" "// ${_name}-BEGIN" _b)
    string(FIND "${_src}" "// ${_name}-END" _e)
    if (_b LESS 0 OR _e LESS 0 OR _e LESS _b)
        message(FATAL_ERROR "shader validation: markers // ${_name}-BEGIN / -END not found in ${_file}")
    endif()
    math(EXPR _len "${_e} - ${_b}")
    string(SUBSTRING "${_src}" ${_b} ${_len} _block)

    string(FIND "${_block}" "R\"(" _rs)
    string(FIND "${_block}" ")\";" _re REVERSE)
    if (_rs LESS 0 OR _re LESS 0 OR _re LESS _rs)
        message(FATAL_ERROR "shader validation: no R\"( ... )\" raw string inside the ${_name} block of ${_file}")
    endif()
    math(EXPR _hs "${_rs} + 3")
    math(EXPR _hl "${_re} - ${_hs}")
    string(SUBSTRING "${_block}" ${_hs} ${_hl} _hlsl)
    file(WRITE "${OUT_DIR}/${_name}.hlsl" "${_hlsl}")

    execute_process(
        COMMAND "${FXC}" /nologo /T ${_profile} /E ${_entry} /O3 /Fo "${OUT_DIR}/${_name}.cso" "${OUT_DIR}/${_name}.hlsl"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if (NOT _rc EQUAL 0)
        message(FATAL_ERROR "shader validation: fxc FAILED for ${_name} (${_file}), exit ${_rc}:\n${_out}${_err}")
    endif()
    if (NOT "${_err}" STREQUAL "")
        string(STRIP "${_err}" _err)
        message(STATUS "fxc ${_name}: ${_err}")
    endif()
    message(STATUS "shader validation: fxc ${_profile} OK -- ${_name} (${_file})")
    math(EXPR _n "${_n} + 1")
endforeach()
message(STATUS "shader validation: ${_n} embedded shader block(s) compiled with ${FXC}")
file(WRITE "${STAMP}" "${_n} blocks OK\n")
