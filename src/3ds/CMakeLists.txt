# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# Extraction/memory do not need the NX runtime, LLVM, SDL or an Azahar build tree.
add_library(snx_ctr_board STATIC board/board.c)
set_target_properties(snx_ctr_board PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(snx_ctr_board PRIVATE -Wall -Wextra -Werror=missing-prototypes)
add_library(snx_ctr_memory STATIC memory/memory.c)
target_link_libraries(snx_ctr_memory PUBLIC snx_ctr_board)
add_library(snx_ctr_cpu STATIC cpu/cpu.c)
target_link_libraries(snx_ctr_cpu PUBLIC snx_ctr_memory)
add_library(snx_ctr_loader STATIC loader/loader.c loader/cia.c loader/ncch.c loader/romfs.c loader/blz.c loader/image.c)
target_link_libraries(snx_ctr_loader PUBLIC scarletnx-common OpenSSL::Crypto)
target_compile_definitions(snx_ctr_loader PRIVATE _FILE_OFFSET_BITS=64 _CRT_SECURE_NO_WARNINGS)
add_library(snx_ctr_process STATIC runtime/process.c kernel/kernel.c kernel/thread.c aot/dispatch.c ipc/ipc.c services/srv.c services/fs.c services/apt.c services/ptm.c services/hid.c services/romfs.c services/file.c services/gsp.c services/dsp.c services/cfg.c)
add_library(snx_ctr_gpu STATIC gpu/present.c gpu/transfer.c gpu/pica.c gpu/shader.c gpu/draw.c gpu/raster.c gpu/lighting.c gpu/texture.c "${volk_SOURCE_DIR}/volk.c")
target_include_directories(snx_ctr_gpu PUBLIC "${volk_SOURCE_DIR}")
target_link_libraries(snx_ctr_gpu PUBLIC snx_ctr_memory Vulkan::Headers SDL3::SDL3 ${CMAKE_DL_LIBS})
set_source_files_properties("${volk_SOURCE_DIR}/volk.c" PROPERTIES COMPILE_OPTIONS -w)
# the host's keyboard and gamepads, as HID samples them
add_library(snx_ctr_input STATIC input/input.c)
target_link_libraries(snx_ctr_input PUBLIC SDL3::SDL3)
# the HLE DSP and its SDL3 output
add_library(snx_ctr_audio STATIC audio/dsp.c audio/source.c audio/mixers.c audio/output.c)
target_link_libraries(snx_ctr_audio PUBLIC snx_ctr_memory SDL3::SDL3)
target_link_libraries(snx_ctr_process PUBLIC snx_ctr_cpu snx_ctr_loader scarletnx-filesystem snx_ctr_gpu snx_ctr_audio snx_ctr_input)
include("${PROJECT_SOURCE_DIR}/cmake/swipc.cmake")
snx_swipc(snx_ctr_process CTR ROOT "${CMAKE_CURRENT_SOURCE_DIR}" FILES services/srv.swipc services/fs.swipc services/apt.swipc services/ptm.swipc services/hid.swipc services/file.swipc)
foreach(target snx_ctr_memory snx_ctr_loader snx_ctr_cpu snx_ctr_process snx_ctr_gpu snx_ctr_audio snx_ctr_input)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror=missing-prototypes)
endforeach()
add_executable(scarletnx-3ds runtime/main.c)
target_link_libraries(scarletnx-3ds PRIVATE snx_ctr_board snx_ctr_loader)
# its checkout, which it works in as the Switch driver does (snx_data_dir)
target_compile_definitions(scarletnx-3ds PRIVATE SNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}" SNX_BUILD_DIR="${CMAKE_BINARY_DIR}")
set_target_properties(scarletnx-3ds PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_compile_options(scarletnx-3ds PRIVATE -Wall -Wextra -Werror=missing-prototypes)
include(GNUInstallDirs)
include("${PROJECT_SOURCE_DIR}/cmake/llvm.cmake")
if(NOT TARGET snx_arm_llvm)
    add_subdirectory("${PROJECT_SOURCE_DIR}/src/common/cpu/arm" ctr-arm-backend)
endif()
set(ctr_runtime_source "${CMAKE_CURRENT_BINARY_DIR}/ctr-runtime-$<CONFIG>.c")
add_custom_command(OUTPUT "${ctr_runtime_source}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUT=$<TARGET_FILE:snx_arm_rt>" "-DOUTPUT=${ctr_runtime_source}"
        -P "${PROJECT_SOURCE_DIR}/cmake/ctr-runtime.cmake"
    DEPENDS snx_arm_rt "${PROJECT_SOURCE_DIR}/cmake/ctr-runtime.cmake" VERBATIM)
# The offline compiler is part of the driver, as the Switch driver's recompiler is part of scarletnx
target_sources(scarletnx-3ds PRIVATE aot/compiler.c aot/discover.c "${ctr_runtime_source}")
target_link_libraries(scarletnx-3ds PRIVATE snx_arm_llvm)
# The compiler loads the LLVM toolchain's own libraries (including its z3)
# at startup: resolve them without a caller-provided environment.
set(ctr_llvm_rpath "${SNX_LLVM_LIBRARY_DIR}")
find_library(SNX_Z3_LIBRARY NAMES z3 libz3.so.4
    HINTS "${SNX_LLVM_LIBRARY_DIR}" "${SNX_LLVM_LIBRARY_DIR}/../.."
    PATH_SUFFIXES lib x86_64-linux-gnu)
if(SNX_Z3_LIBRARY)
    get_filename_component(ctr_z3_dir "${SNX_Z3_LIBRARY}" DIRECTORY)
    string(APPEND ctr_llvm_rpath ":${ctr_z3_dir}")
    # Forced into the dependency table (--as-needed would drop unreferenced
    # libraries): libLLVM's own z3/xml2 dependencies then resolve through
    # the binary's search path instead of the caller's environment.
    find_library(SNX_XML2_LIBRARY NAMES xml2 libxml2.so.2 HINTS "${ctr_z3_dir}" "${SNX_LLVM_LIBRARY_DIR}"
                    PATH_SUFFIXES lib x86_64-linux-gnu)
    target_link_libraries(scarletnx-3ds PRIVATE "-Wl,--no-as-needed" "${SNX_Z3_LIBRARY}" ${SNX_XML2_LIBRARY}
                        "-Wl,--as-needed")
endif()
target_link_options(scarletnx-3ds PRIVATE "-Wl,-rpath,${ctr_llvm_rpath}")
target_compile_definitions(scarletnx-3ds PRIVATE "SNX_CTR_HOST_CC=\"${CMAKE_C_COMPILER}\"")
add_library(snx_ctr_native STATIC aot/load.c)
target_link_libraries(snx_ctr_native PUBLIC snx_ctr_process ${CMAKE_DL_LIBS})
set_target_properties(snx_ctr_native PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(snx_ctr_native PRIVATE -Wall -Wextra -Werror=missing-prototypes)
target_sources(scarletnx-3ds PRIVATE aot/run.c)
target_link_libraries(scarletnx-3ds PRIVATE snx_ctr_native)
install(TARGETS scarletnx-3ds RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(WIN32)
    target_compile_definitions(scarletnx-3ds PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
