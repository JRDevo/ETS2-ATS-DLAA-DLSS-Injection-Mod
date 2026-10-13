# Build-time check of the HLSL compute shaders embedded in the C++ sources (v0.5.6).
# Run as a script:  cmake -DFXC=<fxc.exe> -DSRC_DIR=<repo> -DOUT_DIR=<dir> -DBLOCKS=<list> -DSTAMP=<file>
#                         [-DEMBED_CPP=<file.cpp>] -P validate_shaders.cmake
#   BLOCKS = comma-separated "relative/source.cpp@kBlockName" entries.
#   v0.7.0: an entry may add "@profile@entry" (e.g. "src/scene_dlaa.cpp@kCompositeVs@vs_5_0@VSMain") for the
#   graphics shaders; without it the block is cs_5_0 / CSMain as before.
# Each block is the text between the marker comments
#   // kBlockName-BEGIN ...      and      // kBlockName-END
# and inside that, the C++ raw string  R"( ... )"  is the HLSL. It is written to OUT_DIR/kBlockName.hlsl and
# compiled with fxc (profile + entry as given, default cs_5_0 / CSMain, same as the runtime D3DCompile). Any
# failure stops the build.
#
# Issue #15 (Proton): with EMBED_CPP set, the bytecode of every entry (OUT_DIR/kBlockName__entry__profile.cso) is also
# written into EMBED_CPP as byte arrays + an EmbeddedShaders::kBlobs table (see src/embedded_shaders.h). The table key is
# what changes the compiler output: the exact HLSL source (length + FNV-1a 64), entry, profile and the compile flags.
# The source hash is NOT computed here: the generated file repeats each block's raw string (with its  )" R"(  splits,
# so the C++ compiler sees exactly the token sequence of the original definition) inside a constexpr hash, so the hash
# is taken over exactly the bytes the runtime registers with ShaderCache::Add (sizeof(kBlock) - 1), whatever the line
# endings of the checkout. The literal text itself is only used in the constant evaluation and is not kept in the DLL.
# fxc flags = the runtime D3DCompile flags: /O3 = D3DCOMPILE_OPTIMIZATION_LEVEL3, no defines, no includes.
cmake_minimum_required(VERSION 3.20)   # script mode: policies (IN_LIST) like the main project
foreach(_v FXC SRC_DIR OUT_DIR BLOCKS STAMP)
    if (NOT DEFINED ${_v} OR "${${_v}}" STREQUAL "")
        message(FATAL_ERROR "validate_shaders.cmake: ${_v} not set")
    endif()
endforeach()

file(MAKE_DIRECTORY "${OUT_DIR}")
string(REPLACE "," ";" _blocks "${BLOCKS}")
set(_n 0)
set(_seen_blocks "")          # block names already written to EMBED_CPP (one hash per block, several entries)
set(_cpp_src "")              # per-block source id constants
set(_cpp_blob "")             # per-entry byte arrays
set(_cpp_table "")            # table rows
set(_total_bytes 0)
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

    # one .cso per entry (several entries share a block: kObjShader, kDrawIdCs, kFwdInitShader, kMenuVs)
    set(_cso "${OUT_DIR}/${_name}__${_entry}__${_profile}.cso")
    execute_process(
        COMMAND "${FXC}" /nologo /T ${_profile} /E ${_entry} /O3 /Fo "${_cso}" "${OUT_DIR}/${_name}.hlsl"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if (NOT _rc EQUAL 0)
        message(FATAL_ERROR "shader validation: fxc FAILED for ${_name} (${_file}), exit ${_rc}:\n${_out}${_err}")
    endif()
    if (NOT "${_err}" STREQUAL "")
        string(STRIP "${_err}" _err)
        message(STATUS "fxc ${_name}: ${_err}")
    endif()
    message(STATUS "shader validation: fxc ${_profile} OK -- ${_name} / ${_entry} (${_file})")
    math(EXPR _n "${_n} + 1")

    if (DEFINED EMBED_CPP AND NOT "${EMBED_CPP}" STREQUAL "")
        if (NOT _name IN_LIST _seen_blocks)
            list(APPEND _seen_blocks "${_name}")
            string(APPEND _cpp_src "// ${_name} (${_file})\nconstexpr SrcId kSrc_${_name} = IdOf(R\"(${_hlsl})\");\n\n")
        endif()
        file(READ "${_cso}" _hex HEX)
        string(LENGTH "${_hex}" _hexlen)
        math(EXPR _bytes "${_hexlen} / 2")
        if (_bytes LESS 4)
            message(FATAL_ERROR "shader embed: ${_cso} is empty")
        endif()
        math(EXPR _total_bytes "${_total_bytes} + ${_bytes}")
        # 32 bytes per line: break the hex text every 64 digits, then turn each digit pair into "0xNN,"
        string(REPEAT "[0-9a-f]" 64 _line_re)
        string(REGEX REPLACE "(${_line_re})" "\\1\n" _hex "${_hex}")
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _hex "${_hex}")
        set(_var "kCode_${_name}__${_entry}__${_profile}")
        string(APPEND _cpp_blob "alignas(16) const unsigned char ${_var}[${_bytes}] = {\n${_hex}\n};\n")
        string(APPEND _cpp_table
            "    { \"${_name}\", \"${_entry}\", \"${_profile}\", kFlagsO3, kSrc_${_name}.len, kSrc_${_name}.hash, ${_var}, ${_bytes}u },\n")
    endif()
endforeach()
message(STATUS "shader validation: ${_n} embedded shader block(s) compiled with ${FXC}")

if (DEFINED EMBED_CPP AND NOT "${EMBED_CPP}" STREQUAL "")
    list(LENGTH _seen_blocks _nblocks)
    file(WRITE "${EMBED_CPP}"
"// GENERATED by cmake/validate_shaders.cmake -- do not edit. Issue #15: the prebuilt DXBC bytecode of every shader block
// listed in CMakeLists.txt (_shader_blocks), compiled with ${FXC}
// (/O3 = D3DCOMPILE_OPTIMIZATION_LEVEL3, no defines, the runtime D3DCompile flags). ${_n} entries from ${_nblocks} block(s),
// ${_total_bytes} bytes of bytecode. Looked up by src/shader_cache.cpp.
#ifdef WITH_DLAA
#include \"${SRC_DIR}/src/embedded_shaders.h\"

namespace {
using EmbeddedShaders::SrcId;
using EmbeddedShaders::IdOf;
constexpr uint32_t kFlagsO3 = 1u << 15;   // D3DCOMPILE_OPTIMIZATION_LEVEL3 (fxc /O3)

${_cpp_src}
${_cpp_blob}
}  // namespace

namespace EmbeddedShaders {
const Blob kBlobs[] = {
${_cpp_table}};
const size_t kBlobCount = sizeof(kBlobs) / sizeof(kBlobs[0]);
}  // namespace EmbeddedShaders
#endif // WITH_DLAA
")
    message(STATUS "shader embed: ${_n} bytecode blob(s) (${_total_bytes} bytes) from ${_nblocks} block(s) -> ${EMBED_CPP}")
endif()
file(WRITE "${STAMP}" "${_n} blocks OK\n")
