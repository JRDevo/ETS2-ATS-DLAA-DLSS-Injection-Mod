cmake_minimum_required(VERSION 3.20)
project(ets2_ats_dlaa VERSION 0.13.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Static C/C++ runtime (/MT, /MTd in Debug) for every target (dinput8 + minhook_static), issue #14: with the DLL CRT
# (/MD) the mod needs a VC++ redistributable at least as new as the compiler. Under Proton (and on Windows with an old
# redist) an older msvcp140.dll crashes in std::mutex (VS 17.10+ constexpr mutex constructor, page fault at 0x8).
# CMP0091 is NEW with cmake_minimum_required 3.20, so this variable is honoured. Must be set before the targets.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Ships as dinput8.dll (proxy loader). ETS2/ATS load dinput8 for input, so this
# is a reliable, low-risk injection vector with only 5 exports to forward.
add_library(dinput8 SHARED
    src/proxy.cpp
    src/inject.cpp
    src/ofxr.cpp      # v0.10.0: OFXR Bridge module-range pass-through (compiles with or without WITH_DLAA)
    src/dinput_wrap.cpp   # v0.10.0: tuning menu keys hidden from the game (DirectInput keyboard wrap + window subclass)
    src/xr_hook.cpp       # v0.10.3 phase 27: OpenXR xrReleaseSwapchainImage hook (VR panel drawn right before the release)
    src/vrs.cpp           # phase 34: VRS fixed foveated rendering (NVAPI with WITH_VRS, a "not built in" stub without)
    src/frame_pace.cpp    # phase 52: frame cap + NVIDIA Reflex (NVAPI with WITH_NVAPI = WITH_VRS, the own limiter without)
)
set_target_properties(dinput8 PROPERTIES PREFIX "" OUTPUT_NAME "dinput8")
# Single source of the version string: project(... VERSION 0.10.0) above feeds the
# load-log line in src/inject.cpp through this compile definition. Bump it in project()
# only; the doc headers are the only other place the version is written by hand.
target_compile_definitions(dinput8 PRIVATE DLAA_INJECTOR_VERSION="${PROJECT_VERSION}")
# --- MinHook (Present detour) --- fetched at configure time (needs git +
# internet on first configure). We hook dxgi's Present with a trampoline
# because the Steam overlay inline-detours it and re-enters via the vtable.
# SOURCE_SUBDIR points at a nonexistent dir so MinHook's own CMakeLists is NOT
# used; we compile its 4 sources ourselves into a static lib that inherits our
# static /MT CRT (CMAKE_MSVC_RUNTIME_LIBRARY above) and gets /W0 so its warnings don't pollute our /W3 build.
include(FetchContent)
FetchContent_Declare(minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        v1.3.4   # c3fcafdc10146beb5919319d0683e44e3c30d537
    SOURCE_SUBDIR  _no_cmake_here)
FetchContent_MakeAvailable(minhook)
add_library(minhook_static STATIC
    ${minhook_SOURCE_DIR}/src/hook.c
    ${minhook_SOURCE_DIR}/src/buffer.c
    ${minhook_SOURCE_DIR}/src/trampoline.c
    ${minhook_SOURCE_DIR}/src/hde/hde64.c)
target_include_directories(minhook_static PUBLIC ${minhook_SOURCE_DIR}/include)
if (MSVC)
    target_compile_options(minhook_static PRIVATE /W0)
endif()

# version: GetFileVersionInfo for the v0.7.10 startup "env:" lines (game exe / nvngx_dlss.dll file versions).
# dxguid: the DirectInput IIDs / GUID_SysKeyboard for the v0.10.0 keyboard wrapper (dinput_wrap.cpp).
target_link_libraries(dinput8 PRIVATE d3d11 dxgi minhook_static version dxguid)

if (MSVC)
    target_compile_options(dinput8 PRIVATE /W3 /permissive- /Zc:__cplusplus)
    # Export the 5 dinput8 entry points named in the .def.
    target_link_options(dinput8 PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/dinput8_proxy.def")
endif()

# --- VRS (NVIDIA NVAPI) --- phase 34 fixed foveated rendering. ON automatically when external/nvapi exists (a clone of
# github.com/NVIDIA/nvapi, git-ignored like external/DLSS; MIT), OFF otherwise: src/vrs.cpp then builds as a stub that logs
# "not built in" and the menu row says so. Override with -DWITH_VRS=OFF/ON and/or -DNVAPI_SDK_DIR=path/to/nvapi.
# nvapi64.lib is linked STATICALLY as the SDK intends (it loads nvapi64.dll from the driver at NvAPI_Initialize).
set(NVAPI_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/nvapi" CACHE PATH
    "Path to a clone of github.com/NVIDIA/nvapi (has nvapi.h and amd64/nvapi64.lib)")
if (NOT NVAPI_SDK_DIR)
    set(NVAPI_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/nvapi" CACHE PATH "" FORCE)
endif()
if (EXISTS "${NVAPI_SDK_DIR}/nvapi.h" AND EXISTS "${NVAPI_SDK_DIR}/amd64/nvapi64.lib")
    set(_vrs_default ON)
else()
    set(_vrs_default OFF)
endif()
option(WITH_VRS "Build the NVAPI VRS foveated rendering (needs NVAPI_SDK_DIR)" ${_vrs_default})
if (WITH_VRS)
    if (NOT EXISTS "${NVAPI_SDK_DIR}/nvapi.h" OR NOT EXISTS "${NVAPI_SDK_DIR}/amd64/nvapi64.lib")
        message(FATAL_ERROR "WITH_VRS=ON needs NVAPI_SDK_DIR pointing at a clone of github.com/NVIDIA/nvapi")
    endif()
    target_include_directories(dinput8 PRIVATE "${NVAPI_SDK_DIR}")
    target_compile_definitions(dinput8 PRIVATE WITH_VRS=1 WITH_NVAPI=1)   # phase 52: WITH_NVAPI = Reflex (frame_pace.cpp)
    target_link_libraries(dinput8 PRIVATE "${NVAPI_SDK_DIR}/amd64/nvapi64.lib")
endif()
message(STATUS "VRS (NVAPI foveated rendering): ${WITH_VRS} (${NVAPI_SDK_DIR})")

# --- DLAA (NGX/DLSS) --- ON automatically when external/DLSS exists (a clone of
# github.com/NVIDIA/DLSS, git-ignored: NVIDIA license), OFF otherwise so the base
# build has no external deps. Override with -DWITH_DLAA=OFF/ON and/or
#   -DNGX_SDK_DIR=path/to/DLSS
# At runtime the game also needs nvngx_dlss.dll (from <sdk>/lib/Windows_x86_64/rel)
# next to dinput8.dll or the game exe. See docs/DLAA_INTEGRATION.md.
set(NGX_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/DLSS" CACHE PATH
    "Path to a clone of github.com/NVIDIA/DLSS (has include/ and lib/)")
if (NOT NGX_SDK_DIR)   # stale empty cache entry from an older configure
    set(NGX_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/DLSS" CACHE PATH "" FORCE)
endif()
if (EXISTS "${NGX_SDK_DIR}/include/nvsdk_ngx.h")
    set(_dlaa_default ON)
else()
    set(_dlaa_default OFF)
endif()
option(WITH_DLAA "Build the NGX/DLSS DLAA processor (needs NGX_SDK_DIR)" ${_dlaa_default})

if (WITH_DLAA)
    if (NOT EXISTS "${NGX_SDK_DIR}/include/nvsdk_ngx.h")
        message(FATAL_ERROR "WITH_DLAA=ON needs NGX_SDK_DIR pointing at a clone of github.com/NVIDIA/DLSS")
    endif()
    # _s = static CRT (/MT, our runtime since issue #14); _s_dbg matches /MTd in Debug. (_d / _d_dbg = the /MD ones.)
    # NGX_LIB_STATIC_* are new cache names so an old build dir that cached the _d libs does not keep linking them.
    find_library(NGX_LIB_STATIC_RELEASE nvsdk_ngx_s     PATHS "${NGX_SDK_DIR}/lib/Windows_x86_64/x64" NO_DEFAULT_PATH)
    find_library(NGX_LIB_STATIC_DEBUG   nvsdk_ngx_s_dbg PATHS "${NGX_SDK_DIR}/lib/Windows_x86_64/x64" NO_DEFAULT_PATH)
    if (NOT NGX_LIB_STATIC_RELEASE)
        message(FATAL_ERROR "nvsdk_ngx_s.lib not found under ${NGX_SDK_DIR}/lib/Windows_x86_64/x64")
    endif()
    # v0.7.8: shader_cache.cpp compiles every embedded shader once per process on a worker thread (no D3DCompile on
    # the render thread any more; the blocks below are still fxc-validated at build time).
    target_sources(dinput8 PRIVATE src/dlaa.cpp src/motion_vectors.cpp src/scene_dlaa.cpp src/preview_blit.cpp
                                   src/shader_cache.cpp src/draw_ids.cpp    # v0.10.0: draw_ids = per-draw MVs
                                   src/gpu_perf.cpp                         # v0.10.0 phase 8: per-sub-pass GPU timers
                                   src/menu.cpp                             # v0.10.0 in-game tuning menu (GDI panel quad)
                                   src/hidden_mask.cpp                      # phase 40: VR hidden area mask (depth-only draw)
                                   src/fps_cab.cpp)                         # phase 47: the VR fps box attached to the vehicle
    target_include_directories(dinput8 PRIVATE "${NGX_SDK_DIR}/include")
    target_compile_definitions(dinput8 PRIVATE WITH_DLAA=1)
    # d3dcompiler: runtime HLSL compile fallback (src/shader_cache.cpp) for a shader that has no embedded bytecode
    # (SHADER_VALIDATION=OFF builds, or a source changed without the build step). Issue #15: DELAY-loaded, so the DLL
    # loads and runs with no d3dcompiler_47.dll at all (Proton's builtin one rejects 29 of our shaders) when every
    # shader is embedded; shader_cache LoadLibrary-checks it before the first D3DCompile call.
    # advapi32/user32: pulled by the NGX static lib (registry + window queries);
    # CMake's default MSVC libs cover them, listed here so that stays true.
    target_link_libraries(dinput8 PRIVATE
        "$<IF:$<CONFIG:Debug>,${NGX_LIB_STATIC_DEBUG},${NGX_LIB_STATIC_RELEASE}>"
        d3dcompiler delayimp advapi32 user32)
    if (MSVC)
        target_link_options(dinput8 PRIVATE "/DELAYLOAD:d3dcompiler_47.dll")
    endif()

    # --- embedded HLSL validation (v0.5.6) --- every "// kName-BEGIN ... // kName-END" block in the
    # sources is extracted and compiled with fxc (cs_5_0 / CSMain, like the runtime D3DCompile; v0.7.0: or the
    # "@profile@entry" given after the block name, e.g. vs_5_0 / ps_5_0 for the upscale composite) before
    # dinput8 builds; a shader error fails the build instead of showing up as a runtime log line.
    # Issue #15: the same step also keeps the DXBC bytecode and generates <build>/shaders/embedded_shaders.cpp (every
    # blob as a byte array + a table keyed by source hash / length, entry, profile, compile flags); shader_cache.cpp uses
    # those blobs instead of D3DCompile (Wine/Proton's builtin d3dcompiler rejects most of our shaders). Every shader the
    # runtime registers (ShaderCache::Add) must be listed in _shader_blocks with the same entry + profile, or it falls
    # back to D3DCompile at runtime (logged).
    # Register new blocks in _shader_blocks. -DSHADER_VALIDATION=OFF skips it (no Windows SDK fxc): no embedded bytecode
    # then, every shader is compiled at runtime as before.
    option(SHADER_VALIDATION "Compile the embedded HLSL blocks with fxc at build time" ON)
    if (SHADER_VALIDATION)
        if (NOT FXC_EXE)
            set(_pf86 "ProgramFiles(x86)")
            file(GLOB _fxc_found "$ENV{${_pf86}}/Windows Kits/10/bin/*/x64/fxc.exe")
            list(SORT _fxc_found)
            list(REVERSE _fxc_found)              # newest SDK first
            set(_fxc_dirs "")
            foreach(_f IN LISTS _fxc_found)
                get_filename_component(_d "${_f}" DIRECTORY)
                list(APPEND _fxc_dirs "${_d}")
            endforeach()
            find_program(FXC_EXE NAMES fxc.exe fxc PATHS ${_fxc_dirs} NO_DEFAULT_PATH)
            find_program(FXC_EXE NAMES fxc.exe fxc)
        endif()
        if (NOT FXC_EXE)
            message(FATAL_ERROR "fxc.exe not found (Windows 10/11 SDK). Set -DFXC_EXE=path or -DSHADER_VALIDATION=OFF")
        endif()
        message(STATUS "shader validation: fxc = ${FXC_EXE}")
        set(_shader_blocks
            "src/scene_dlaa.cpp@kDepthConvertShader"
            "src/scene_dlaa.cpp@kMvDebugShader"
            "src/scene_dlaa.cpp@kRcasShader"
            "src/preview_blit.cpp@kPvBlitVs@vs_5_0@VSMain"      # v0.7.8 profile-screen preview format-converting copy
            "src/preview_blit.cpp@kPvBlitPs@ps_5_0@PSMain"
            "src/hidden_mask.cpp@kHmVs@vs_5_0@VSMain"           # phase 40 VR hidden area mask (depth only)
            "src/menu.cpp@kMenuVs@vs_5_0@VSMain"                # v0.10.0 tuning menu panel quad
            "src/menu.cpp@kMenuPs@ps_5_0@PSMain"
            "src/menu.cpp@kMenuVs@vs_5_0@VSCab"                 # phase 47 the fps box attached to the vehicle
            "src/scene_dlaa.cpp@kCompositeVs@vs_5_0@VSMain"     # v0.7.0 upscale composite (graphics)
            "src/scene_dlaa.cpp@kCompositePs@ps_5_0@PSMain"
            "src/motion_vectors.cpp@kSolveShader"
            "src/motion_vectors.cpp@kReprojDepthShader"
            "src/motion_vectors.cpp@kObjShader"                 # v0.9.0 per-object MVs (gather + verdict + id R)
            "src/motion_vectors.cpp@kObjShader@cs_5_0@CSResolve" # v0.9.0 r4 per-id representative
            "src/motion_vectors.cpp@kObjShader@cs_5_0@CSVeto"   # v0.9.0 r4 same-frame id veto
            "src/motion_vectors.cpp@kObjShader@cs_5_0@CSHold"   # v0.9.0 r7 per-id veto verdict / hold
            "src/draw_ids.cpp@kDrawIdPs@ps_5_0@PSMain"          # v0.10.0 draw-id replay pixel shader
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSGather"        # v0.10.0 per-draw MVs: MVP gather
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSMatch"         # v0.10.0 nearest partner (both sides)
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSResolve"       # v0.10.0 mutual pairs -> R per draw
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSPair"          # v0.10.0 phase 3 mutual pair + R_draw
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSVote"          # v0.10.0 phase 3 consensus votes
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSPick"          # v0.10.0 phase 3 consensus camera R
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSAttach"        # v0.10.0 phase 4 forward draws on a G-buffer mover
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSInherit1"      # v0.10.0 phase 5 twin table + G-buffer mover list
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSTwin"          # v0.10.0 phase 5 unpaired draws inherit their matrix twin
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSParentList"    # v0.10.0 phase 6 unpaired draws -> vote slots
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSParentVote"    # v0.10.0 phase 6 pixel-neighbour rigid-parent vote
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSParentPick1"   # v0.10.0 phase 6 rigid parent -> R
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSParentPick2"   # v0.10.0 phase 6 parent through an unpaired neighbour
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSParentCount"   # v0.10.0 phase 6b instanced-draw pixel count (vote slots)
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSPickResolve"   # v0.10.0 phase 9 folded: pick + resolve + inherit (1 group)
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSListTwin"      # v0.10.0 phase 9 folded: parent list + twin + attach (1 group)
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSPickPar"       # v0.10.0 phase 18: consensus pick, statistic spread over a group
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSResolveInherit" # v0.10.0 phase 18: resolve + inherit, one wide dispatch
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSParentCollect" # v0.10.0 phase 18: compact vote, march items of the listed tiles
            "src/draw_ids.cpp@kDrawIdCs@cs_5_0@CSParentMarch"   # v0.10.0 phase 18: one thread per march item + direction
            "src/draw_ids.cpp@kFwdInitShader@vs_5_0@VSMain"     # v0.10.0 phase 10 forward-depth occlusion init (full-screen tri)
            "src/draw_ids.cpp@kFwdInitShader@ps_5_0@PSInit0"    #   full resolution
            "src/draw_ids.cpp@kFwdInitShader@ps_5_0@PSInit1"    #   1/2 resolution (min of 2x2)
            "src/draw_ids.cpp@kBiasNormShader@ps_5_0@PSNorm"    # phase 49 glow bias mask: glow share of the pixel
            "src/scene_dlaa.cpp@kBiasPrepShader"                # phase 49 crop + last frame's glow along the MVs
            "src/inject.cpp@kPvStatsShader"                     # v0.7.8 Ctrl+F10 preview capture metrics
            "src/inject.cpp@kPvDepthAsmShader"                  # v0.7.8 tiled preview picture depth
            "src/inject.cpp@kPvEdgeFillShader")                 # v0.8.1 preview tile edge (zero-guarded fill)
        string(REPLACE ";" "," _shader_blocks_arg "${_shader_blocks}")
        set(_shader_stamp "${CMAKE_CURRENT_BINARY_DIR}/shaders/validated.stamp")
        set(_shader_embed_cpp "${CMAKE_CURRENT_BINARY_DIR}/shaders/embedded_shaders.cpp")   # issue #15
        add_custom_command(
            OUTPUT  "${_shader_stamp}" "${_shader_embed_cpp}"
            COMMAND "${CMAKE_COMMAND}"
                    "-DFXC=${FXC_EXE}"
                    "-DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                    "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/shaders"
                    "-DBLOCKS=${_shader_blocks_arg}"
                    "-DSTAMP=${_shader_stamp}"
                    "-DEMBED_CPP=${_shader_embed_cpp}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/validate_shaders.cmake"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/scene_dlaa.cpp"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/motion_vectors.cpp"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/preview_blit.cpp"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/hidden_mask.cpp"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/menu.cpp"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/inject.cpp"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/draw_ids.cpp"
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/validate_shaders.cmake"
            COMMENT "Validating embedded HLSL shader blocks with fxc + embedding their bytecode"
            VERBATIM)
        add_custom_target(validate_shaders ALL DEPENDS "${_shader_stamp}" "${_shader_embed_cpp}")
        add_dependencies(dinput8 validate_shaders)
        # issue #15: the prebuilt DXBC table (shader_cache.cpp looks it up when DLAA_EMBEDDED_SHADERS is defined).
        # The generated file hashes each HLSL raw string in a constexpr (so the literal text itself is not kept in the
        # DLL); the biggest block is ~116 KB, past MSVC's default constexpr step budget.
        target_sources(dinput8 PRIVATE "${_shader_embed_cpp}")
        set_source_files_properties("${_shader_embed_cpp}" PROPERTIES GENERATED TRUE
            COMPILE_OPTIONS "/constexpr:steps100000000")
        target_compile_definitions(dinput8 PRIVATE DLAA_EMBEDDED_SHADERS=1)
    endif()
endif()
