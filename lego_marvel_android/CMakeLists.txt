cmake_minimum_required(VERSION 3.20)
project(LegoMarvelPS3Android
    VERSION 0.1.0
    LANGUAGES C CXX ASM
    DESCRIPTION "LEGO Marvel Super Heroes (BLUS-31161) Native Android ARM64 Port (ps3recomp)"
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Essential for linking libps3recomp_runtime.a into libps3recomp_lego_marvel.so
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT DEFINED PS3RECOMP_DIR)
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ps3recomp" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/recompiled" CACHE PATH
    "Directory containing lifted PPU output (ppu_recomp.h + ppu_recomp_*.cpp)")
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/spu" CACHE PATH
    "Directory containing lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")

# Default to smoke/bring-up mode when decrypted EBOOT.ELF has not been lifted yet
if(EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    set(_default_smoke OFF)
else()
    set(_default_smoke ON)
endif()
option(ANDROID_SMOKE_MODE "Build interactive Android smoke/bring-up title instead of lifted EBOOT.ELF" ${_default_smoke})
option(BUILD_ANDROID_CLI "Also build CLI runner binary for shell verification" ON)

# Force Vulkan RSX + AArch64 fibers for Android
set(PS3RECOMP_RSX_VULKAN ON CACHE BOOL "" FORCE)
set(PS3RECOMP_BUILD_TESTS OFF CACHE BOOL "" FORCE)

add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk" EXCLUDE_FROM_ALL)
if(TARGET ps3recomp_runtime)
    target_compile_definitions(ps3recomp_runtime PRIVATE RSX_DRAW_ENGINE_TEXTURE_CACHE=4096)
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)

if(ANDROID_SMOKE_MODE)
    message(STATUS "Mode: ANDROID_SMOKE_MODE=ON (interactive Android Vulkan/AAudio/Pad/PPU bring-up)")
    set(ACTIVE_RECOMP_DIR "${CMAKE_CURRENT_BINARY_DIR}/smoke_gen")
    add_custom_command(
        OUTPUT "${ACTIVE_RECOMP_DIR}/smoke.elf" "${ACTIVE_RECOMP_DIR}/ppu_recomp.h"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${ACTIVE_RECOMP_DIR}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/make_smoke_elf.py"
                --out    "${ACTIVE_RECOMP_DIR}/smoke.elf"
                --header "${ACTIVE_RECOMP_DIR}/ppu_recomp.h"
        DEPENDS "${PS3RECOMP_DIR}/tools/make_smoke_elf.py"
                "${PS3RECOMP_DIR}/tools/ppu_lifter.py"
        COMMENT "Generating bring-up smoke.elf and ppu_recomp.h"
    )
    set(ACTIVE_RECOMP_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/src/android_smoke_recomp.c"
        "${ACTIVE_RECOMP_DIR}/ppu_recomp.h"
    )
    set(SPU_SOURCES "")
else()
    message(STATUS "Mode: FULL LIFTED TITLE (${RECOMP_DIR})")
    set(ACTIVE_RECOMP_DIR "${RECOMP_DIR}")
    file(GLOB ACTIVE_RECOMP_SOURCES CONFIGURE_DEPENDS
        "${RECOMP_DIR}/*.c"
        "${RECOMP_DIR}/*.cpp"
    )
    list(FILTER ACTIVE_RECOMP_SOURCES EXCLUDE REGEX "ppu_hle_nids\\.cpp$")
    if(NOT ACTIVE_RECOMP_SOURCES)
        message(FATAL_ERROR "No lifted sources found in ${RECOMP_DIR}. Run scripts/lift_and_build.sh <EBOOT.ELF> first.")
    endif()

    set(SPU_SOURCES "")
    if(EXISTS "${SPU_DIR}/sources.cmake")
        include("${SPU_DIR}/sources.cmake")
        if(SPU_GENERATED_SOURCES)
            list(APPEND SPU_SOURCES ${SPU_GENERATED_SOURCES} "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        endif()
    else()
        file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
        if(SPU_LIFTED)
            list(APPEND SPU_SOURCES ${SPU_LIFTED} "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
            foreach(gen IN ITEMS spu_workloads.c jobmods_register.c taskset_policy_bytes.c)
                if(EXISTS "${SPU_DIR}/${gen}")
                    list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
                endif()
            endforeach()
        endif()
    endif()
    if(SPU_SOURCES)
        set_source_files_properties(${SPU_SOURCES} PROPERTIES
            INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
            COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT
            COMPILE_OPTIONS "-O2;-w"
        )
    endif()
    set_source_files_properties(${ACTIVE_RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-O1;-w;-ffp-contract=off"
    )
endif()

# Generate or reuse the full 1,250-handler HLE NID dispatch table
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp" AND NOT ANDROID_SMOKE_MODE)
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
else()
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    file(GLOB_RECURSE HLE_NID_SOURCES CONFIGURE_DEPENDS "${PS3RECOMP_DIR}/libs/*.c")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
                ${HLE_NID_SOURCES}
        COMMENT "Generating HLE NID -> handler dispatch table"
    )
endif()

set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

set(COMMON_INCLUDES
    "${ACTIVE_RECOMP_DIR}"
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"
    "${PS3RECOMP_DIR}/libs/video"
    "${PS3RECOMP_DIR}/libs/input"
    "${PS3RECOMP_DIR}/templates/project"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

set(COMMON_COMPILE_FLAGS
    -Wall -Wextra -Wno-unused-parameter
    -fno-strict-aliasing
    -fwrapv
    -ffp-contract=off
    -march=armv8-a
)

# Shared PIC OBJECT library so lifted PPU/SPU TUs + scaffold compile ONCE
add_library(lego_marvel_core OBJECT
    src/stubs.cpp
    src/port_extension.cpp
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${ACTIVE_RECOMP_SOURCES}
    ${SPU_SOURCES}
)
target_include_directories(lego_marvel_core PRIVATE ${COMMON_INCLUDES})
target_compile_options(lego_marvel_core PRIVATE ${COMMON_COMPILE_FLAGS})
if(ANDROID_SMOKE_MODE)
    target_compile_definitions(lego_marvel_core PRIVATE ANDROID_SMOKE_MODE=1)
endif()

# Optional AdrenoTools custom Vulkan driver loader (Mesa Turnip / Qualcomm .zip)
set(ADRENOTOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libadrenotools")
set(HAVE_ADRENOTOOLS OFF)
if(EXISTS "${ADRENOTOOLS_DIR}/CMakeLists.txt")
    set(CMAKE_ANDROID_ARCH_ABI "arm64-v8a")
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory("${ADRENOTOOLS_DIR}" "${CMAKE_BINARY_DIR}/adrenotools_build" EXCLUDE_FROM_ALL)
    set(HAVE_ADRENOTOOLS ON)
endif()

# ---------------------------------------------------------------------------
# Target 1: Native Android APK Shared Library (libps3recomp_lego_marvel.so)
# ---------------------------------------------------------------------------
add_library(ps3recomp_lego_marvel SHARED
    src/android_main.cpp
    $<TARGET_OBJECTS:lego_marvel_core>
)
target_include_directories(ps3recomp_lego_marvel PRIVATE ${COMMON_INCLUDES})
target_compile_options(ps3recomp_lego_marvel PRIVATE ${COMMON_COMPILE_FLAGS})
if(ANDROID_SMOKE_MODE)
    target_compile_definitions(ps3recomp_lego_marvel PRIVATE ANDROID_SMOKE_MODE=1)
endif()
target_link_directories(ps3recomp_lego_marvel PRIVATE /system/lib64)
target_link_libraries(ps3recomp_lego_marvel PRIVATE
    ps3recomp_runtime
    android
    log
    m
    dl
)
if(HAVE_ADRENOTOOLS)
    target_compile_definitions(ps3recomp_lego_marvel PRIVATE HAVE_ADRENOTOOLS=1)
    target_link_libraries(ps3recomp_lego_marvel PRIVATE adrenotools)
    add_dependencies(ps3recomp_lego_marvel hook_impl main_hook file_redirect_hook gsl_alloc_hook)
endif()
# Export ANativeActivity_onCreate and GameActivity JNI HUD symbols cleanly
target_link_options(ps3recomp_lego_marvel PRIVATE
    "-Wl,--export-dynamic-symbol=ANativeActivity_onCreate"
    "-Wl,--export-dynamic-symbol=Java_com_ps3recomp_legomarvel_GameActivity_nativeGetFrameStats"
    "-Wl,--export-dynamic-symbol=Java_com_ps3recomp_legomarvel_GameActivity_nativeGetRenderHeight"
    "-Wl,--export-dynamic-symbol=Java_com_ps3recomp_legomarvel_GameActivity_nativeGetVirtualPadState"
)
set_target_properties(ps3recomp_lego_marvel PROPERTIES LINKER_LANGUAGE CXX)

# ---------------------------------------------------------------------------
# Target 2: Optional CLI Runner (for shell / headless testing on Android)
# ---------------------------------------------------------------------------
if(BUILD_ANDROID_CLI)
    add_executable(lego_marvel_cli
        src/android_main.cpp
        $<TARGET_OBJECTS:lego_marvel_core>
    )
    target_include_directories(lego_marvel_cli PRIVATE ${COMMON_INCLUDES})
    target_compile_options(lego_marvel_cli PRIVATE ${COMMON_COMPILE_FLAGS})
    target_compile_definitions(lego_marvel_cli PRIVATE PS3RECOMP_ANDROID_CLI=1)
    if(ANDROID_SMOKE_MODE)
        target_compile_definitions(lego_marvel_cli PRIVATE ANDROID_SMOKE_MODE=1)
    endif()
    target_link_libraries(lego_marvel_cli PRIVATE
        ps3recomp_runtime
        m
        dl
    )
    set_target_properties(lego_marvel_cli PROPERTIES LINKER_LANGUAGE CXX)
endif()
