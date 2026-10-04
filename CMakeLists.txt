cmake_minimum_required(VERSION 3.20)
project(ets2_ats_dlaa VERSION 0.7.6 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Ships as dinput8.dll (proxy loader). ETS2/ATS load dinput8 for input, so this
# is a reliable, low-risk injection vector with only 5 exports to forward.
add_library(dinput8 SHARED
    src/proxy.cpp
    src/inject.cpp
)
set_target_properties(dinput8 PROPERTIES PREFIX "" OUTPUT_NAME "dinput8")
# Single source of the version string: project(... VERSION 0.7.6) above feeds the
# load-log line in src/inject.cpp through this compile definition. Bump it in project()
# only; the doc headers are the only other place the version is written by hand.
target_compile_definitions(dinput8 PRIVATE DLAA_INJECTOR_VERSION="${PROJECT_VERSION}")
# --- MinHook (Present detour) --- fetched at configure time (needs git +
# internet on first configure). We hook dxgi's Present with a trampoline
# because the Steam overlay inline-detours it and re-enters via the vtable.
# SOURCE_SUBDIR points at a nonexistent dir so MinHook's own CMakeLists is NOT
# used; we compile its 4 sources ourselves into a static lib that inherits our
# default /MD CRT and gets /W0 so its warnings don't pollute our /W3 build.
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

target_link_libraries(dinput8 PRIVATE d3d11 dxgi minhook_static)

if (MSVC)
    target_compile_options(dinput8 PRIVATE /W3 /permissive- /Zc:__cplusplus)
    # Export the 5 dinput8 entry points named in the .def.
    target_link_options(dinput8 PRIVATE "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/src/dinput8_proxy.def")
endif()

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
    # _d = dynamic CRT (/MD, our default); _dbg variants match /MDd in Debug.
    find_library(NGX_LIB_RELEASE nvsdk_ngx_d     PATHS "${NGX_SDK_DIR}/lib/Windows_x86_64/x64" NO_DEFAULT_PATH)
    find_library(NGX_LIB_DEBUG   nvsdk_ngx_d_dbg PATHS "${NGX_SDK_DIR}/lib/Windows_x86_64/x64" NO_DEFAULT_PATH)
    if (NOT NGX_LIB_RELEASE)
        message(FATAL_ERROR "nvsdk_ngx_d.lib not found under ${NGX_SDK_DIR}/lib/Windows_x86_64/x64")
    endif()
    target_sources(dinput8 PRIVATE src/dlaa.cpp src/motion_vectors.cpp src/scene_dlaa.cpp)
    target_include_directories(dinput8 PRIVATE "${NGX_SDK_DIR}/include")
    target_compile_definitions(dinput8 PRIVATE WITH_DLAA=1)
    # d3dcompiler: runtime HLSL compile for the depth + motion-vector passes (OS DLL).
    # advapi32/user32: pulled by the NGX static lib (registry + window queries);
    # CMake's default MSVC libs cover them, listed here so that stays true.
    target_link_libraries(dinput8 PRIVATE
        "$<IF:$<CONFIG:Debug>,${NGX_LIB_DEBUG},${NGX_LIB_RELEASE}>"
        d3dcompiler advapi32 user32)

    # --- embedded HLSL validation (v0.5.6) --- every "// kName-BEGIN ... // kName-END" block in the
    # sources is extracted and compiled with fxc (cs_5_0 / CSMain, like the runtime D3DCompile; v0.7.0: or the
    # "@profile@entry" given after the block name, e.g. vs_5_0 / ps_5_0 for the upscale composite) before
    # dinput8 builds; a shader error fails the build instead of showing up as a runtime log line.
    # Register new blocks in _shader_blocks. -DSHADER_VALIDATION=OFF skips it (no Windows SDK fxc).
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
            "src/scene_dlaa.cpp@kCompositeVs@vs_5_0@VSMain"     # v0.7.0 upscale composite (graphics)
            "src/scene_dlaa.cpp@kCompositePs@ps_5_0@PSMain"
            "src/motion_vectors.cpp@kSolveShader"
            "src/motion_vectors.cpp@kReprojDepthShader")
        string(REPLACE ";" "," _shader_blocks_arg "${_shader_blocks}")
        set(_shader_stamp "${CMAKE_CURRENT_BINARY_DIR}/shaders/validated.stamp")
        add_custom_command(
            OUTPUT  "${_shader_stamp}"
            COMMAND "${CMAKE_COMMAND}"
                    "-DFXC=${FXC_EXE}"
                    "-DSRC_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
                    "-DOUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/shaders"
                    "-DBLOCKS=${_shader_blocks_arg}"
                    "-DSTAMP=${_shader_stamp}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/validate_shaders.cmake"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/scene_dlaa.cpp"
                    "${CMAKE_CURRENT_SOURCE_DIR}/src/motion_vectors.cpp"
                    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/validate_shaders.cmake"
            COMMENT "Validating embedded HLSL shader blocks with fxc"
            VERBATIM)
        add_custom_target(validate_shaders ALL DEPENDS "${_shader_stamp}")
        add_dependencies(dinput8 validate_shaders)
    endif()
endif()
