option(EDEN_MKW_DIAGNOSTICS "Enable Mario Kart development controls and capture instrumentation" OFF)
option(EDEN_SCRIPTED_INPUT "Enable file-driven input for console qualification" OFF)
option(EDEN_PS5_VULKAN "Build the native Vulkan renderer alongside OpenGL" OFF)
set(EDEN_VULKAN_DRIVER "CUSTOM" CACHE STRING "Native Vulkan driver: CUSTOM or RADV")
set_property(CACHE EDEN_VULKAN_DRIVER PROPERTY STRINGS CUSTOM RADV)
if(NOT EDEN_VULKAN_DRIVER MATCHES "^(CUSTOM|RADV)$")
    message(FATAL_ERROR "EDEN_VULKAN_DRIVER must be CUSTOM or RADV")
endif()
set(PORT_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
set(PORT_BUILD_DIR "${CMAKE_BINARY_DIR}/headless")
file(MAKE_DIRECTORY "${PORT_BUILD_DIR}")
# What the port derived from the core before Eglantine/ came into this repository: a build folder
# from then still has it, and the headers under include/ would hide the core's own.
foreach(stale include/core include/video_core include/common include/dynarmic sparse macro-native
        recovery pipeline-wait vulkan-cache multiplayer)
    file(REMOVE_RECURSE "${PORT_BUILD_DIR}/${stale}")
endforeach()
function(write_derived path content)
    # Preserve timestamps when an overlay is unchanged; otherwise every
    # configure needlessly recompiles hundreds of dependent source files.
    file(WRITE "${path}.in" "${content}")
    configure_file("${path}.in" "${path}" COPYONLY)
endfunction()
# The network and core code in Eglantine/ include the port's multiplayer headers
# (headless/multiplayer_*.h) by name; the packets they exchange are compressed with zstd.
if(TARGET network)
    target_include_directories(network PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
endif()
target_include_directories(core PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(core PRIVATE zstd::zstd)
# Keep successful JIT invariant checks inline; preserve upstream failure handling.
# Use a private include root so other core/renderer targets retain their headers.
file(READ "${PROJECT_SOURCE_DIR}/src/common/assert.h" jit_assert_header)
file(READ "${CMAKE_CURRENT_LIST_DIR}/jit_assert.inc" jit_assert_override)
write_derived("${PORT_BUILD_DIR}/jit-checks/common/assert.h"
    "${jit_assert_header}\n${jit_assert_override}")
target_include_directories(dynarmic BEFORE PRIVATE "${PORT_BUILD_DIR}/jit-checks")
# Let the existing compiler inline across the JIT's translation units. Keep the
# rest of the native application on its qualified non-LTO build path.
target_compile_options(dynarmic PRIVATE -flto=thin)
if(PS5_NATIVE)
    # The GPU thread's per-draw work (command processing, buffer/texture caches) spans
    # video_core's translation units, and the console's CPU is a Zen 2 (AVX2, BMI2, LZCNT/TZCNT).
    # Not common or core: with them a title never reached its first frame (their fiber switches
    # move guest threads between host threads, which cross-unit inlining may not survive).
    # EDEN_CPU_PRESET=zen2 (ps5.cmake) passes the same -march to every target; repeated here it
    # changes nothing.
    target_compile_options(video_core PRIVATE -flto=thin -march=znver2)
endif()
if(NOT PS5_NATIVE)
    target_link_options(dynarmic INTERFACE -fuse-ld=lld-18)
endif()

# Clang 18 does not accept the upstream GCC TLS-dialect flag. Use the same
# compiler-rt emulated TLS configuration already qualified by G6 on PS5.
# Linux must match its system libstdc++'s native TLS ABI.
foreach(eden_target common core audio_core video_core hid_core network input_common frontend_common shader_recompiler gpu_logging dynarmic)
    if(TARGET ${eden_target})
        get_target_property(options ${eden_target} COMPILE_OPTIONS)
        if(options)
            list(FILTER options EXCLUDE REGEX "mtls-dialect")
            set_property(TARGET ${eden_target} PROPERTY COMPILE_OPTIONS "${options}")
        endif()
        if(PS5_NATIVE)
            target_compile_options(${eden_target} PRIVATE -femulated-tls)
        endif()
    endif()
endforeach()
if(PS5_NATIVE)
    set(COMPILER_RT "${PORT_ROOT}/.deps/compiler-rt-18.1.8")
    file(READ "${COMPILER_RT}/emutls.c" emutls)
    string(REPLACE "#define EMUTLS_SKIP_DESTRUCTOR_ROUNDS 0" "#define EMUTLS_SKIP_DESTRUCTOR_ROUNDS 1" emutls "${emutls}")
    write_derived("${PORT_BUILD_DIR}/emutls.c" "${emutls}")
    target_sources(common PRIVATE "${PORT_BUILD_DIR}/emutls.c")
    target_include_directories(common PRIVATE "${COMPILER_RT}")
endif()
# ReShade's effect compiler takes alloca from <malloc.h>, which the Payload SDK replaces by an
# #error. Its own copy of that header, for this target alone.
if(PS5_NATIVE AND TARGET reshadefx)
    write_derived("${PORT_BUILD_DIR}/reshadefx-compat/malloc.h"
        "#pragma once\n#include <stdlib.h>\n#ifndef alloca\n#define alloca __builtin_alloca\n#endif\n")
    target_include_directories(reshadefx BEFORE PRIVATE "${PORT_BUILD_DIR}/reshadefx-compat")
endif()
# Use the already-qualified no-arena allocation contract in the full core.
get_target_property(common_sources common SOURCES)
list(REMOVE_ITEM common_sources host_memory.cpp sparse_large_vector.cpp)
set_property(TARGET common PROPERTY SOURCES "${common_sources}")
target_sources(common PRIVATE "${PORT_ROOT}/src/host_memory.cpp" "${PORT_ROOT}/src/memory_pages.cpp")
file(READ "${PROJECT_SOURCE_DIR}/src/common/host_memory.h" port_host_memory_header)
if(port_host_memory_header MATCHES "enum class BackingMode")
    target_compile_definitions(common PRIVATE EDEN_HOST_MEMORY_BACKING_MODE=1)
endif()
if(port_host_memory_header MATCHES "void MarkUsed\\(")
    target_compile_definitions(common PRIVATE EDEN_HOST_MEMORY_ZERO_TRACKING=1)
endif()
if(port_host_memory_header MATCHES "void HintCold\\(")
    target_compile_definitions(common PRIVATE EDEN_HOST_MEMORY_COLD_HINT=1)
endif()


get_target_property(jit_sources dynarmic SOURCES)
list(FILTER jit_sources EXCLUDE REGEX "exception_handler_posix\\.cpp$")
set_property(TARGET dynarmic PROPERTY SOURCES "${jit_sources}")
# A32 fastmem: src/host_memory.cpp aliases 32-bit guest memory into a 4 GiB window at
# the PS5's 16 KiB page size; this handler sends faulting JIT accesses to their
# page-table fallback. Without it the generic handler keeps fastmem off (and the JIT's memory
# emitters in Eglantine/ take their checked path: EDEN_A32_FASTMEM, headless/inject.cmake).
if(EDEN_A32_FASTMEM AND (PS5_NATIVE OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    target_sources(dynarmic PRIVATE "${EDEN_PORT_DIR}/fastmem_handler.cpp")
else()
    target_sources(dynarmic PRIVATE "${PROJECT_SOURCE_DIR}/src/dynarmic/src/dynarmic/backend/exception_handler_generic.cpp")
endif()
if(PS5_NATIVE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # The port's xbyak (jit-alias.cmake) is a generated header the JIT includes.
    target_include_directories(dynarmic BEFORE PUBLIC "${PORT_BUILD_DIR}/include")
    include("${EDEN_PORT_DIR}/jit-alias.cmake")
    # Layouts the JIT (Eglantine/src/dynarmic) and the port's code agree on: compiled-block ranges by
    # 4 KiB guest page, decoders that parse once, one page lookup for nearby A64 accesses, and the
    # JIT state that lets any core run code another core compiled (one compiled-code namespace per
    # guest process).
    target_compile_definitions(dynarmic PRIVATE EDEN_BLOCK_RANGE_PAGES=1 EDEN_DECODER_ONE_PARSE=1
        EDEN_PAGE_REUSE=1 EDEN_SHARED_JIT_LAYOUT=1)
endif()

# Core's NS query service owns this implementation from frontend_common.
target_sources(core PRIVATE "${PROJECT_SOURCE_DIR}/src/frontend_common/play_time_manager.cpp")
# Replace SDL audio with the optional native sink; the renderer stays null.
target_compile_definitions(audio_core PRIVATE EDEN_NATIVE_AUDIO_SINK=1)
get_target_property(audio_sources audio_core SOURCES)
list(FILTER audio_sources EXCLUDE REGEX "sink/sdl3_sink\\.")
set_property(TARGET audio_core PROPERTY SOURCES "${audio_sources}")
get_target_property(audio_definitions audio_core COMPILE_DEFINITIONS)
list(REMOVE_ITEM audio_definitions HAVE_SDL3)
set_property(TARGET audio_core PROPERTY COMPILE_DEFINITIONS "${audio_definitions}")
get_target_property(audio_libraries audio_core LINK_LIBRARIES)
list(REMOVE_ITEM audio_libraries SDL3::SDL3)
set_property(TARGET audio_core PROPERTY LINK_LIBRARIES "${audio_libraries}")
get_target_property(audio_interface audio_core INTERFACE_LINK_LIBRARIES)
list(REMOVE_ITEM audio_interface SDL3::SDL3 "$<LINK_ONLY:SDL3::SDL3>")
set_property(TARGET audio_core PROPERTY INTERFACE_LINK_LIBRARIES "${audio_interface}")

# Reuse the independently authored native ABI contracts from the research.
set(pad_contract "${PORT_ROOT}/third_party")
set(audio_contract "${PORT_ROOT}/third_party")
file(SHA256 "${pad_contract}/ps5_pad.hpp" pad_hash)
file(SHA256 "${audio_contract}/native_audio.hpp" audio_hash)
if(NOT pad_hash STREQUAL "6a9708fe7ab59afa69899e1981e84decf1492a97e8ebd08194f8e27841a00ee0" OR
   NOT audio_hash STREQUAL "54627379f641d608a9a5c6a96ebdfc543a0dbb8a1e843e0ce08cb6e3adb93528")
    message(FATAL_ERROR "Native device contracts changed; review the adapter ABI first.")
endif()
# pad_haptics.cpp: HD rumble through the DualSense audio haptics (sceAudioOut vibration port).
add_library(eden-pad STATIC "${EDEN_PORT_DIR}/pad.cpp" "${EDEN_PORT_DIR}/pad_haptics.cpp"
    "${PROJECT_SOURCE_DIR}/src/input_common/input_engine.cpp"
    "${PROJECT_SOURCE_DIR}/src/input_common/input_poller.cpp"
    "${PROJECT_SOURCE_DIR}/src/input_common/drivers/virtual_gamepad.cpp")
target_include_directories(eden-pad PUBLIC "${EDEN_PORT_DIR}" "${pad_contract}" "${audio_contract}" "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(eden-pad PUBLIC common)
target_compile_options(eden-pad PRIVATE -fno-rtti $<$<BOOL:${PS5_NATIVE}>:-femulated-tls>)
target_sources(audio_core PRIVATE "${EDEN_PORT_DIR}/audio.cpp")
target_include_directories(audio_core PRIVATE "${EDEN_PORT_DIR}" "${pad_contract}" "${audio_contract}")
if(PS5_NATIVE)
    target_link_libraries(eden-pad PUBLIC "${sdk}/target/lib/libScePad.so" "${sdk}/target/lib/libSceUserService.so"
        "${sdk}/target/lib/libSceAudioOut.so")
    target_link_libraries(audio_core PUBLIC "${sdk}/target/lib/libSceAudioOut.so")
else()
    add_library(eden-device-mock STATIC "${EDEN_PORT_DIR}/mock_devices.cpp")
    target_include_directories(eden-device-mock PUBLIC "${EDEN_PORT_DIR}" "${pad_contract}" "${audio_contract}")
    target_include_directories(eden-device-mock PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(eden-device-mock PRIVATE common)
    target_link_libraries(audio_core PUBLIC eden-device-mock)
endif()

get_target_property(video_sources video_core SOURCES)
if(NOT EDEN_PS5_VULKAN)
    target_compile_definitions(video_core PRIVATE EDEN_NO_VULKAN=1)
    # Without Vulkan (the host's test build): Eglantine/src/video_core/video_core.cpp leaves the
    # Vulkan renderer out there too.
    list(FILTER video_sources EXCLUDE REGEX "^(renderer_vulkan|vulkan_common)/")
else()
    include("${EDEN_PORT_DIR}/vulkan.cmake")
endif()
# The native macro engine (headless/macro_native*.h) hashes macro code with OpenSSL.
target_link_libraries(video_core PUBLIC OpenSSL::Crypto)
set_property(TARGET video_core PROPERTY SOURCES "${video_sources}")

if(PS5_NATIVE)
    target_compile_definitions(libzstd_static PRIVATE ZSTD_TRACE=0)
    # Prefer this build's matching decoder headers over the C dependency
    # bundle's older FFmpeg headers exposed through OpenSSL's include root.
    target_include_directories(video_core BEFORE PRIVATE "${FFmpeg_INCLUDE_DIR}")
    # Eden only uses Boost.Process v1's header-only async_pipe. Its unused v2
    # library assumes newer FreeBSD process APIs than the PS5 SDK supplies.
    foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
        get_target_property(libraries core ${property})
        list(REMOVE_ITEM libraries Boost::process)
        set_property(TARGET core PROPERTY ${property} "${libraries}")
    endforeach()
    target_include_directories(core PRIVATE "$<TARGET_PROPERTY:boost_process,INTERFACE_INCLUDE_DIRECTORIES>")
    target_link_libraries(core PRIVATE "$<TARGET_PROPERTY:boost_process,INTERFACE_LINK_LIBRARIES>")
    set(native_template "${PORT_ROOT}/../ps5-native-app-boilerplate")
    set(heap_source "${PORT_ROOT}/third_party/app_heap.c")
    file(SHA256 "${heap_source}" heap_hash)
    if(NOT heap_hash STREQUAL "df61acb51502c3b75c4fda62859c42eac6d19a6690f9b68c0e2dfbf20ae72c0f")
        message(FATAL_ERROR "The shared heap changed since G6; qualify it before using it here.")
    endif()
    # The heap may grow to 6 GiB: with guest cores 1/2 at 192 MiB of code, a large game's
    # blocks (~0.5 KiB of metadata each) once exhausted 2 GiB, and Tears of the Kingdom with the
    # UltraCam mod filled 3 GiB building the mod's RomFS while the console had 8 GiB free
    # (Swordpdf/ProsperoEden#246). That is its address range, not its memory: the heap commits
    # 128 MiB pieces as allocations need them (heap_arenas.inc), because everything the CPU holds
    # comes out of the 12 GiB it shares with the GPU. Taken whole at start, guest RAM, a dense page
    # table, a 3 GiB heap and the A64 code caches were 8.6 GiB of it; where no range can be
    # reserved, the heap still takes only 3 GiB at once (EDEN_HEAP_WHOLE_SIZE). The size no longer
    # fits an unsigned int, so it is a size_t, and so is the one line that prints it.
    # (The stock wrappers, EDEN_HEAP_ARENAS off, take the heap whole: 3 GiB there.)
    option(EDEN_HEAP_ARENAS "Per-thread arenas inside the owned heap" ON)
    set(heap_mib 3072)
    if(EDEN_HEAP_ARENAS)
        set(heap_mib 6144)
    endif()
    file(READ "${heap_source}" heap)
    string(REPLACE "128 MiB" "${heap_mib} MiB" heap "${heap}")
    string(REPLACE "(128u * 1024u * 1024u)" "((size_t)${heap_mib} << 20)" heap "${heap}")
    string(REPLACE "base=%p size=%u\\n" "base=%p size=%zu\\n" heap "${heap}")
    string(REPLACE "#include <sys/mman.h>" "#include <sys/mman.h>\nvoid *eden_heap_pages(size_t);\nvoid eden_heap_pages_free(void *, size_t);" heap "${heap}")
    string(REPLACE "mmap(NULL, PS5_OPENGL_HEAP_SIZE, PROT_READ | PROT_WRITE,\n                    MAP_PRIVATE | MAP_ANON, -1, 0)" "eden_heap_pages(PS5_OPENGL_HEAP_SIZE)" heap "${heap}")
    string(REPLACE "if (base == MAP_FAILED)" "if (base == NULL)" heap "${heap}")
    string(REPLACE "munmap(base, PS5_OPENGL_HEAP_SIZE)" "eden_heap_pages_free(base, PS5_OPENGL_HEAP_SIZE)" heap "${heap}")
    string(REPLACE "\"PS5-OpenGL\"" "\"Eden-headless\"" heap "${heap}")
    # Replace the single-mspace wrappers with per-thread arenas over the same heap
    # (headless/heap_arenas.inc). The shared source above stays pinned/unchanged.
    if(EDEN_HEAP_ARENAS)
        set(heap_wrappers_begin "static int ps5_heap_ready(void) {")
        set(heap_wrappers_end "void ps5_opengl_heap_stats_print(unsigned iteration) {")
        string(FIND "${heap}" "${heap_wrappers_begin}" heap_begin_at)
        string(FIND "${heap}" "${heap_wrappers_end}" heap_end_at)
        if(heap_begin_at LESS 0 OR heap_end_at LESS heap_begin_at)
            message(FATAL_ERROR "Pinned heap wrapper layout changed")
        endif()
        string(SUBSTRING "${heap}" 0 ${heap_begin_at} heap_prefix)
        string(SUBSTRING "${heap}" ${heap_end_at} -1 heap_suffix)
        file(READ "${EDEN_PORT_DIR}/heap_arenas.inc" heap_arenas)
        set(heap "${heap_prefix}${heap_arenas}${heap_suffix}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${EDEN_PORT_DIR}/heap_arenas.inc")
    endif()
    option(EDEN_HEAP_GUARD "Development trailing heap corruption guards" OFF)
    if(EDEN_HEAP_GUARD)
        string(REPLACE "static atomic_int ps5_heap_state;"
            "#include \"${EDEN_PORT_DIR}/heap_guard.h\"\nstatic atomic_int ps5_heap_state;" heap "${heap}")
    endif()
    write_derived("${PORT_BUILD_DIR}/app_heap.c" "${heap}")
    set(eden_app_crt "${native_template}/tooling/native/app_crt.cpp")
    if(EDEN_PS5_VULKAN AND EDEN_VULKAN_DRIVER STREQUAL "RADV")
        file(READ "${eden_app_crt}" radv_crt)
        string(FIND "${radv_crt}" "extern \"C\" void _init()\n{" fp_init_at)
        if(fp_init_at EQUAL -1)
            message(FATAL_ERROR "Native CRT initialization changed; review IEEE startup placement")
        endif()
        string(REPLACE "extern \"C\" void _init()\n{"
            "extern \"C\" void ps5_fp_ieee();\nextern \"C\" void _init()\n{\n    ps5_fp_ieee();"
            radv_crt "${radv_crt}")
        set(eden_app_crt "${PORT_BUILD_DIR}/radv_app_crt.cpp")
        write_derived("${eden_app_crt}" "${radv_crt}")
    endif()
    add_executable(eden-headless "${EDEN_PORT_DIR}/main.cpp" "${PORT_ROOT}/src/aligned_alloc.c"
        "${EDEN_PORT_DIR}/heap_pages.cpp" "${EDEN_PORT_DIR}/performance.cpp"
        "${EDEN_PORT_DIR}/crash_report.cpp" "${EDEN_PORT_DIR}/stop_limit.cpp"
        "${EDEN_PORT_DIR}/elevation/elevation.cpp"
        "${eden_app_crt}" "${PORT_ROOT}/src/lifecycle.c" "${PORT_ROOT}/src/direct_memory.c"
        "${PORT_BUILD_DIR}/app_heap.c")
    target_compile_definitions(eden-headless PRIVATE PS5_NATIVE=1
        $<$<BOOL:${EDEN_MKW_DIAGNOSTICS}>:EDEN_MKW_DIAGNOSTICS=1>
        $<$<BOOL:${EDEN_SCRIPTED_INPUT}>:MKW_DIAGNOSTIC_INPUT=1>)
    set_property(TARGET eden-headless APPEND PROPERTY LINK_DEPENDS
        "${PORT_ROOT}/tools/link-headless-native.sh" "${PORT_ROOT}/tools/unwind.ld"
        "${PORT_ROOT}/tools/app-symbols.map")
    if(EDEN_PS5_VULKAN AND EDEN_VULKAN_DRIVER STREQUAL "RADV")
        set_property(TARGET eden-headless APPEND PROPERTY LINK_DEPENDS
            "${PORT_ROOT}/tools/radv-link-eden.sh"
            "${PORT_ROOT}/../mihawk-vulkan-review/tools/radv-link.sh")
    endif()
    target_compile_definitions(common PRIVATE PS5_NATIVE=1)
else()
    add_executable(eden-headless "${EDEN_PORT_DIR}/main.cpp" "${PORT_ROOT}/src/aligned_alloc.c"
        "${EDEN_PORT_DIR}/stop_limit.cpp")
    target_link_options(eden-headless PRIVATE -Wl,--wrap=aligned_alloc)
    add_executable(eden-romfs-check "${EDEN_PORT_DIR}/romfs_check.cpp")
    target_include_directories(eden-romfs-check PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(eden-romfs-check PRIVATE core)
    target_compile_options(eden-romfs-check PRIVATE -fno-rtti)
    add_executable(eden-scalar-check "${EDEN_PORT_DIR}/scalar_check.cpp")
    target_include_directories(eden-scalar-check PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(eden-scalar-check PRIVATE dynarmic)
    add_executable(eden-memory-check "${EDEN_PORT_DIR}/memory_check.cpp")
    target_include_directories(eden-memory-check PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(eden-memory-check PRIVATE dynarmic common)
    target_compile_options(eden-scalar-check PRIVATE -fno-rtti)
    add_executable(eden-ryujinx-check "${EDEN_PORT_DIR}/ryujinx_check.cpp")
    add_executable(eden-shader-cache-check "${EDEN_PORT_DIR}/shader_cache_check.cpp")
    target_include_directories(eden-shader-cache-check PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(eden-shader-cache-check PRIVATE video_core)
    target_compile_options(eden-shader-cache-check PRIVATE -fno-rtti)
    add_executable(eden-nup-check "${EDEN_PORT_DIR}/nup_check.cpp")
    target_include_directories(eden-nup-check PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(eden-nup-check PRIVATE core)
    target_compile_options(eden-nup-check PRIVATE -fno-rtti)
    add_executable(eden-mods-check "${EDEN_PORT_DIR}/mods_check.cpp")
    target_include_directories(eden-mods-check PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(eden-mods-check PRIVATE core)
    target_compile_options(eden-mods-check PRIVATE -fno-rtti)
    # The settings store's check is written with assert(): they stay in a release build.
    add_executable(eden-settings-check "${EDEN_PORT_DIR}/preferences_check.cpp")
    target_link_libraries(eden-settings-check PRIVATE core)
    target_compile_options(eden-settings-check PRIVATE -UNDEBUG)
    # So is the check of the cheat files' entries (patch_library.h).
    add_executable(eden-patch-library-check "${EDEN_PORT_DIR}/patch_library_check.cpp")
    target_compile_options(eden-patch-library-check PRIVATE -UNDEBUG)
    add_executable(eden-devices-check "${EDEN_PORT_DIR}/devices_check.cpp")
    target_link_libraries(eden-devices-check PRIVATE core eden-pad eden-device-mock)
    target_compile_options(eden-devices-check PRIVATE -fno-rtti)
    # The player profiles (profiles.h): Eden's profile file and the choice in the settings file.
    add_executable(eden-profiles-check "${EDEN_PORT_DIR}/profiles_check.cpp")
    target_link_libraries(eden-profiles-check PRIVATE core)
    target_compile_options(eden-profiles-check PRIVATE -UNDEBUG)
    # save-sync.json kept in step with the profiles (save_sync_config.h).
    add_executable(eden-save-sync-check "${EDEN_PORT_DIR}/save_sync_check.cpp")
    target_link_libraries(eden-save-sync-check PRIVATE core)
    target_compile_options(eden-save-sync-check PRIVATE -UNDEBUG)
    # The update check's decision, without its network side (update_notice.h).
    add_executable(eden-update-check
        "${EDEN_PORT_DIR}/update_check_check.cpp" "${EDEN_PORT_DIR}/update_check/update_check.c")
    target_compile_definitions(eden-update-check PRIVATE UPDATE_CHECK_NO_NETWORK=1)
    target_compile_options(eden-update-check PRIVATE -UNDEBUG)
    # The self-update helper keeps the version it replaces: moving it to where it is kept.
    add_executable(eden-update-files-check
        "${EDEN_PORT_DIR}/self_update_files_check.cpp" "${EDEN_PORT_DIR}/self_update_helper/files.cpp")
    target_include_directories(eden-update-files-check PRIVATE "${EDEN_PORT_DIR}")
endif()
target_include_directories(eden-headless PRIVATE "${PROJECT_SOURCE_DIR}/src")
# The app version has one home, sce_sys/param.json (contentVersion), which the package carries as
# it is. A header of its own, so a new version recompiles only the two files that show it.
file(READ "${PORT_ROOT}/sce_sys/param.json" app_param)
string(JSON app_version GET "${app_param}" contentVersion)
if(NOT app_version MATCHES "^[0-9][0-9]\\.[0-9][0-9][0-9]\\.[0-9][0-9][0-9]$")
    message(FATAL_ERROR "sce_sys/param.json: contentVersion must be NN.NNN.NNN, not '${app_version}'")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PORT_ROOT}/sce_sys/param.json")
write_derived("${PORT_BUILD_DIR}/version/app_version.h"
    "// Generated from sce_sys/param.json (contentVersion).\n#pragma once\n#define EDEN_APP_VERSION \"${app_version}\"\n")
target_include_directories(eden-headless PRIVATE "${PORT_BUILD_DIR}/version")
target_link_libraries(eden-headless PRIVATE core eden-pad)
# A game's text entry on the PS5's own on-screen keyboard (system_keyboard.h, keyboard_applet.h).
target_sources(eden-headless PRIVATE "${EDEN_PORT_DIR}/system_keyboard.cpp")
target_sources(eden-headless PRIVATE "${EDEN_PORT_DIR}/multiplayer.cpp")
target_include_directories(eden-headless BEFORE PRIVATE "${MULTIPLAYER_SOCKET_HEADERS}" "${MULTIPLAYER_OUTPUT}")
if(PS5_NATIVE)
    target_link_libraries(eden-headless PRIVATE
        "${sdk}/target/lib/libSceImeDialog.so" "${sdk}/target/lib/libSceSysmodule.so"
        "${PORT_ROOT}/build/stubs/libSceCommonDialog.so")
    # Games on NFS servers (headless/nfs/nfs.h): libnfs, and the app's layer that passes the C library's file
    # calls through it (the final link wraps them: tools/link-headless-native.sh).
    set(libnfs_dir "${PORT_ROOT}/third_party/libnfs")
    file(GLOB_RECURSE libnfs_sources "${libnfs_dir}/lib/*.c" "${libnfs_dir}/mount/*.c" "${libnfs_dir}/nfs/*.c"
        "${libnfs_dir}/nfs4/*.c" "${libnfs_dir}/portmap/*.c")
    add_library(nfs-client STATIC ${libnfs_sources})
    target_include_directories(nfs-client PUBLIC "${libnfs_dir}" "${libnfs_dir}/include" "${libnfs_dir}/include/nfsc"
        "${libnfs_dir}/mount" "${libnfs_dir}/nfs" "${libnfs_dir}/nfs4" "${libnfs_dir}/portmap")
    target_compile_definitions(nfs-client PRIVATE HAVE_CONFIG_H HAVE_NFS4_2 "_U_=__attribute__((unused))")
    target_compile_options(nfs-client PRIVATE -w -fPIC -ffunction-sections -fdata-sections
        $<$<COMPILE_LANGUAGE:C>:-std=gnu99>)
    # Games on NTFS disks (headless/ntfs/ntfsfs.h): the read-only NTFS reader of PS5-NTFS-BROWSER.
    add_library(ntfs-reader STATIC "${EDEN_PORT_DIR}/ntfs/bdev.c" "${EDEN_PORT_DIR}/ntfs/ntfs.c"
        "${EDEN_PORT_DIR}/ntfs/part.c" "${EDEN_PORT_DIR}/ntfs/utf.c")
    target_compile_options(ntfs-reader PRIVATE -w -fPIC -ffunction-sections -fdata-sections
        $<$<COMPILE_LANGUAGE:C>:-std=gnu11>)
    target_sources(eden-headless PRIVATE "${EDEN_PORT_DIR}/nfs/nfs.cpp" "${EDEN_PORT_DIR}/ntfs/ntfsfs.cpp")
    set_source_files_properties("${EDEN_PORT_DIR}/ntfs/ntfsfs.cpp" PROPERTIES COMPILE_OPTIONS "-Wno-error;-Wno-unused-function")
    target_link_libraries(eden-headless PRIVATE ntfs-reader)
    set_source_files_properties("${EDEN_PORT_DIR}/nfs/nfs.cpp" PROPERTIES COMPILE_OPTIONS "-Wno-error;-Wno-unused-function")
    target_link_libraries(eden-headless PRIVATE nfs-client)
endif()
add_library(eden-metadata STATIC "${EDEN_PORT_DIR}/metadata_bridge.cpp")
target_include_directories(eden-metadata PUBLIC "${EDEN_PORT_DIR}" PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(eden-metadata PUBLIC core stb)
target_compile_options(eden-metadata PRIVATE -fno-rtti -Wall -Wextra -Werror -Wno-unused-parameter)
if(PS5_NATIVE)
    target_compile_options(eden-metadata PRIVATE -femulated-tls)
endif()
add_dependencies(eden-headless eden-metadata)

option(EDEN_HOST_ASAN "Instrument host emulator code for memory diagnostics" OFF)
if(EDEN_HOST_ASAN)
    if(PS5_NATIVE)
        message(FATAL_ERROR "EDEN_HOST_ASAN is a Linux host diagnostic only")
    endif()
    # Generated guest instructions and custom fiber stacks are not covered.
    foreach(asan_target common core audio_core video_core hid_core eden-pad eden-device-mock eden-headless)
        target_compile_options(${asan_target} PRIVATE -fsanitize=address -g1 -fno-omit-frame-pointer
            -fno-sanitize-address-use-after-scope -fsanitize-address-use-after-return=never)
    endforeach()
    target_link_options(eden-headless PRIVATE -fsanitize=address)
endif()
target_compile_options(eden-headless PRIVATE -Wall -Wextra -Werror -Wno-unused-parameter -fwrapv $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>)
if(PS5_NATIVE)
    target_compile_options(eden-headless PRIVATE -femulated-tls)
    option(EDEN_DEVICE_FRONTEND "Enable the unqualified native controller/audio frontend" OFF)
    target_compile_definitions(eden-headless PRIVATE EDEN_DEVICE_FRONTEND=$<BOOL:${EDEN_DEVICE_FRONTEND}>)
endif()

# Consume the frozen, relocatable 4.6 SDK as one complete profile.
option(EDEN_PS5_OPENGL "Use the PS5 EGL/OpenGL frontend" OFF)
set(EDEN_DEV_ROM_ID "" CACHE STRING "Development-only first-launch title ID; empty uses launcher")
if(NOT EDEN_DEV_ROM_ID STREQUAL "")
    string(LENGTH "${EDEN_DEV_ROM_ID}" dev_id_length)
    if(NOT dev_id_length EQUAL 16 OR NOT EDEN_DEV_ROM_ID MATCHES "^[0-9A-Fa-f]+$")
        message(FATAL_ERROR "Development title ID must be 16 hex digits")
    endif()
    target_compile_definitions(eden-headless PRIVATE EDEN_DEV_ROM_ID="${EDEN_DEV_ROM_ID}")
endif()
option(EDEN_DEV_PROFILE "Development-only autoboot, timed input replay and profiling" OFF)
set(EDEN_DEV_PROFILE_TITLE "" CACHE STRING "Title ID the development profile boots and replays input for")
option(EDEN_DEV_WAIT_CALLERS "Development-only bounded GPU-owner caller sampling" OFF)
if(EDEN_DEV_WAIT_CALLERS)
    if(NOT EDEN_DEV_PROFILE OR NOT PS5_NATIVE)
        message(FATAL_ERROR "Caller sampling requires the native development profile")
    endif()
    target_compile_definitions(eden-headless PRIVATE EDEN_DEV_WAIT_CALLERS=1)
endif()
if(EDEN_DEV_PROFILE)
    string(LENGTH "${EDEN_DEV_PROFILE_TITLE}" profile_title_length)
    if(NOT profile_title_length EQUAL 16 OR NOT EDEN_DEV_PROFILE_TITLE MATCHES "^[0-9A-Fa-f]+$")
        message(FATAL_ERROR "EDEN_DEV_PROFILE needs EDEN_DEV_PROFILE_TITLE (a 16-digit title ID)")
    endif()
    target_compile_definitions(eden-headless PRIVATE EDEN_DEV_PROFILE=1
        EDEN_DEV_PROFILE_TITLE="${EDEN_DEV_PROFILE_TITLE}")
endif()
option(EDEN_GPU_PROBE "Run bounded graphics-only architecture matrix, then exit" OFF)
if(EDEN_GPU_PROBE)
    if(NOT EDEN_PS5_OPENGL)
        message(FATAL_ERROR "GPU probe requires native OpenGL")
    endif()
    target_compile_definitions(eden-headless PRIVATE EDEN_GPU_PROBE=1)
endif()
if(EDEN_PS5_OPENGL)
    if(NOT PS5_NATIVE OR NOT ENABLE_OPENGL)
        message(FATAL_ERROR "PS5 graphics requires the native OpenGL build")
    endif()
    # The pinned PS5 OpenGL 4.6 SDK release (tools/deps.json, make deps).
    set(gl46 "${PORT_ROOT}/.deps/ps5-opengl-sdk-1.0.2/sdk")
    set(gl46_expected "7fd5d7a1beb7bf5af42e6a6936b88b1266cba3f224d3a60c3a6a09285e339ae1")
    if(NOT EXISTS "${gl46}/lib/libps5_opengl_core33.a")
        message(FATAL_ERROR "Missing the PS5 OpenGL SDK in ${gl46}; run make deps")
    endif()
    file(SHA256 "${gl46}/lib/libps5_opengl_core33.a" gl46_hash)
    if(EDEN_DEV_PROFILE)
        target_compile_definitions(eden-headless PRIVATE EDEN_DEV_GL_VERSION="${gl46_hash}-sdk-1.0.2")
    endif()
    if(NOT gl46_hash STREQUAL gl46_expected)
        message(FATAL_ERROR "Frozen OpenGL 4.6 runtime changed")
    endif()
    set(PS5OpenGL_DIR "${gl46}/lib/cmake/PS5OpenGL" CACHE PATH "Frozen Eden graphics SDK" FORCE)
    find_package(PS5OpenGL CONFIG REQUIRED PATHS "${gl46}/lib/cmake/PS5OpenGL" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    target_sources(eden-headless PRIVATE "${EDEN_PORT_DIR}/graphics.cpp")
    # The loading screen's shader as a string for the OpenGL window; the Vulkan HUD pass includes
    # the same two files (vulkan_hud.frag).
    file(READ "${EDEN_PORT_DIR}/loading_text.glsl" loading_wordmark)
    file(READ "${EDEN_PORT_DIR}/loading_scene.glsl" loading_scene)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${EDEN_PORT_DIR}/loading_text.glsl" "${EDEN_PORT_DIR}/loading_scene.glsl")
    write_derived("${PORT_BUILD_DIR}/include/loading_scene_glsl.h"
        "// Generated from loading_text.glsl and loading_scene.glsl.\n#pragma once\ninline constexpr const char* kLoadingSceneGlsl = R\"glsl(\n${loading_wordmark}${loading_scene})glsl\";\n")
    target_compile_definitions(eden-headless PRIVATE EDEN_PS5_OPENGL=1)
    target_link_libraries(eden-headless PRIVATE PS5OpenGL::OpenGL glad)
endif()
    # Launcher UI: its own small OpenGL renderer, mixer and screens (headless/eglantineps5/pe),
    # all gone before a game starts.
    if(PS5_NATIVE)
        set(launcher_dir "${EDEN_PORT_DIR}/eglantineps5")
        target_sources(eden-headless PRIVATE
            "${launcher_dir}/runtime_shims.cpp"
            "${launcher_dir}/radio_input.c")
        target_include_directories(eden-headless PRIVATE "${launcher_dir}" "${PORT_ROOT}/third_party")
        if(EDEN_PS5_OPENGL)
            set(launcher_sources
                "${EDEN_PORT_DIR}/multiplayer_directory.cpp"
                "${launcher_dir}/frontend.cpp"
                "${launcher_dir}/eden_services.cpp"
                "${launcher_dir}/pe/audio/mixer.cpp"
                "${launcher_dir}/pe/audio/sounds.cpp"
                "${launcher_dir}/pe/audio/wav.cpp"
                "${launcher_dir}/pe/core/strings.cpp"
                "${launcher_dir}/pe/gfx/bidi.cpp"
                "${launcher_dir}/pe/gfx/draw_list.cpp"
                "${launcher_dir}/pe/gfx/font.cpp"
                "${launcher_dir}/pe/gfx/gl_batch.cpp"
                "${launcher_dir}/pe/gfx/gl_program.cpp"
                "${launcher_dir}/pe/gfx/harfbuzz.cpp"
                "${launcher_dir}/pe/gfx/image.cpp"
                "${launcher_dir}/pe/gfx/system_fonts.cpp"
                "${launcher_dir}/pe/platform/audio_out.cpp"
                "${launcher_dir}/pe/platform/display_egl.cpp"
                "${launcher_dir}/pe/ui/browse.cpp"
                "${launcher_dir}/pe/ui/game_options.cpp"
                "${launcher_dir}/pe/ui/profiles.cpp"
                "${launcher_dir}/pe/ui/remote.cpp"
                "${launcher_dir}/pe/ui/save_sync.cpp"
                "${launcher_dir}/pe/ui/pairing.cpp"
                "${launcher_dir}/pe/ui/multiplayer.cpp"
                "${launcher_dir}/pe/ui/qr_code.cpp"
                "${launcher_dir}/pe/ui/update.cpp"
                "${launcher_dir}/pe/ui/home.cpp"
                "${launcher_dir}/pe/ui/launcher.cpp"
                "${launcher_dir}/pe/ui/library.cpp"
                "${launcher_dir}/pe/ui/settings.cpp"
                "${launcher_dir}/pe/ui/textures.cpp"
                "${launcher_dir}/pe/ui/theme.cpp"
                "${launcher_dir}/pe/ui/widgets.cpp")
            target_sources(eden-headless PRIVATE ${launcher_sources})
            # Download sources and their queue (remote/remote.h), with their backends (remote/source.h,
            # backends.h) and the console's FTP server that writes the files (remote/ftp.h); their
            # HTTP (remote/http.c) is linked into the update check's object below, with its libcurl.
            target_sources(eden-headless PRIVATE
                "${EDEN_PORT_DIR}/remote/remote.cpp"
                "${EDEN_PORT_DIR}/remote/backends.cpp"
                "${EDEN_PORT_DIR}/remote/ftp.cpp"
                "${EDEN_PORT_DIR}/remote/stream_check.cpp"
                "${EDEN_PORT_DIR}/remote/save_archive.cpp"
                "${EDEN_PORT_DIR}/remote/save_sync.cpp"
                "${EDEN_PORT_DIR}/remote/romm/romm_client.cpp"
                "${EDEN_PORT_DIR}/remote/romm/romm_saves.cpp"
                "${EDEN_PORT_DIR}/remote/romm/romm_source.cpp")
            # The save data's zips (remote/save_archive.h): miniz, as the self-update helper has it.
            add_library(eglantineps5-miniz STATIC
                "${PORT_ROOT}/third_party/miniz/miniz.c"
                "${PORT_ROOT}/third_party/miniz/miniz_tdef.c"
                "${PORT_ROOT}/third_party/miniz/miniz_tinfl.c"
                "${PORT_ROOT}/third_party/miniz/miniz_zip.c")
            target_include_directories(eglantineps5-miniz PUBLIC "${PORT_ROOT}/third_party/miniz")
            target_compile_options(eglantineps5-miniz PRIVATE -w)
            target_link_libraries(eden-headless PRIVATE eglantineps5-miniz)
            # Their MD5 is OpenSSL's (linked below for the update check), as is the SHA-256 a
            # download's contents are checked with (remote/stream_check.h). Its include folder, for
            # these files only: the prefix it is in has older FFmpeg headers as well.
            set_source_files_properties("${EDEN_PORT_DIR}/remote/save_archive.cpp"
                "${EDEN_PORT_DIR}/remote/stream_check.cpp"
                PROPERTIES INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}")
            # The update check (update_notice.h). Its network side is the boilerplate's
            # update_check.c and console_curl.c with libcurl and libpsl from the pinned PacBrew
            # prefix that already supplies OpenSSL and zlib (zstd is Eden's own). They are linked
            # into one object first, and what console_curl.c puts in place of libc for libcurl
            # (name lookup, fcntl on sockets, a few more functions) gets a name of its own there
            # (console_curl_getaddrinfo, ...): the emulator's network and file code keeps the
            # functions it had, so the update check changes nothing else. The boilerplate wraps
            # fcntl for the whole program (--wrap=fcntl); here only this object's calls go
            # through the wrapper.
            get_filename_component(update_libraries "${OPENSSL_SSL_LIBRARY}" DIRECTORY)
            if(NOT EXISTS "${update_libraries}/libcurl.a" OR NOT EXISTS "${update_libraries}/libpsl.a")
                message(FATAL_ERROR "libcurl is missing from the PacBrew prefix (${update_libraries}): run make deps")
            endif()
            find_program(UPDATE_CHECK_LINKER NAMES ld.lld-18 ld.lld REQUIRED)
            find_program(UPDATE_CHECK_OBJCOPY NAMES llvm-objcopy-18 llvm-objcopy REQUIRED)
            add_library(update-check OBJECT
                "${EDEN_PORT_DIR}/update_check/update_check.c"
                "${EDEN_PORT_DIR}/update_check/console_curl.c"
                "${EDEN_PORT_DIR}/update_check/self_update.c"
                "${EDEN_PORT_DIR}/update_check/self_update_ps5.c"
                "${EDEN_PORT_DIR}/update_check/self_update_sha256.c"
                "${EDEN_PORT_DIR}/remote/http.c")
            target_include_directories(update-check PRIVATE "${OPENSSL_INCLUDE_DIR}" "${EDEN_PORT_DIR}/update_check")
            # The kit's paths are the app's own: after elevation /app0 and /download0 are gone
            # (update_notice.cpp gives them).
            target_compile_definitions(update-check PRIVATE CURL_STATICLIB=1
                "SELF_UPDATE_HELPER_PATH=eden_self_update_path(0)"
                "SELF_UPDATE_PARAM_PATH=eden_self_update_path(1)"
                "SELF_UPDATE_SEQUENCE_PATH=eden_self_update_path(2)")
            set_source_files_properties("${EDEN_PORT_DIR}/update_check/self_update_ps5.c"
                PROPERTIES COMPILE_OPTIONS "-include;${EDEN_PORT_DIR}/update_check/eden_paths.h")
            # Every function console_curl.c defines under a libc name.
            set(update_own
                getaddrinfo freeaddrinfo gai_strerror gethostbyname getnameinfo fnmatch getpwuid_r
                _setjmp _longjmp openlog closelog dladdr if_nametoindex pipe2 recvmmsg sendmmsg
                popen pclose isatty mkstemp gmtime_r ZSTD_trace_compress_begin
                ZSTD_trace_compress_end ZSTD_trace_decompress_begin ZSTD_trace_decompress_end)
            set(update_rename --redefine-sym fcntl=__wrap_fcntl --redefine-sym __real_fcntl=fcntl)
            foreach(name IN LISTS update_own)
                list(APPEND update_rename --redefine-sym "${name}=console_curl_${name}")
            endforeach()
            set(update_object "${CMAKE_CURRENT_BINARY_DIR}/update-check.o")
            add_custom_command(OUTPUT "${update_object}"
                COMMAND "${UPDATE_CHECK_LINKER}" -r -o "${update_object}.all"
                    "$<TARGET_OBJECTS:update-check>"
                    "${update_libraries}/libcurl.a" "${update_libraries}/libpsl.a"
                COMMAND "${UPDATE_CHECK_OBJCOPY}" ${update_rename}
                    "${update_object}.all" "${update_object}"
                DEPENDS update-check "$<TARGET_OBJECTS:update-check>"
                    "${update_libraries}/libcurl.a" "${update_libraries}/libpsl.a"
                COMMAND_EXPAND_LISTS VERBATIM)
            set_source_files_properties("${update_object}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
            target_sources(eden-headless PRIVATE "${EDEN_PORT_DIR}/update_notice.cpp" "${update_object}")
            target_link_libraries(eden-headless PRIVATE
                "${OPENSSL_SSL_LIBRARY}" "${OPENSSL_CRYPTO_LIBRARY}" "${ZLIB_LIBRARY}")
            # The launcher calls OpenGL by name (the SDK's headers), not through glad.
            set(launcher_definitions "GL_GLEXT_PROTOTYPES=1")
            if(EXISTS "${EDEN_PORT_DIR}/ryujinx_saves.h")
                list(APPEND launcher_definitions "EDEN_SAVE_IMPORT=1")
            endif()
            set_source_files_properties(${launcher_sources}
                PROPERTIES COMPILE_DEFINITIONS "${launcher_definitions}")
            # Text in the scripts the baked font lacks: the console's own fonts, shaped by HarfBuzz
            # (a pinned source release, tools/deps.json) and drawn through stb_truetype.
            set(harfbuzz_dir "${PORT_ROOT}/.deps/harfbuzz-12.3.2/src")
            if(NOT EXISTS "${harfbuzz_dir}/harfbuzz.cc")
                message(FATAL_ERROR "HarfBuzz is missing (${harfbuzz_dir}): run make deps")
            endif()
            set_source_files_properties(
                "${launcher_dir}/pe/gfx/system_fonts.cpp" "${launcher_dir}/pe/gfx/harfbuzz.cpp"
                PROPERTIES INCLUDE_DIRECTORIES "${harfbuzz_dir};${PORT_ROOT}/tools/launcher/stb")
            # HarfBuzz is not ours to tidy: its warnings are off.
            set_source_files_properties("${launcher_dir}/pe/gfx/harfbuzz.cpp" PROPERTIES COMPILE_OPTIONS "-w")
        else()
            target_sources(eden-headless PRIVATE "${launcher_dir}/frontend_stub.cpp")
        endif()
        target_link_libraries(eden-headless PRIVATE eden-metadata)
    endif()

if(EDEN_PS5_VULKAN)
    # Both backends import the same system module; use the superset facade.
    get_target_property(gl_imports PS5OpenGL::OpenGL INTERFACE_LINK_LIBRARIES)
    list(FILTER gl_imports EXCLUDE REGEX "/libSceAgcDriver[.]so$")
    list(APPEND gl_imports "${PORT_ROOT}/build/stubs/libSceAgcDriver.so")
    set_property(TARGET PS5OpenGL::OpenGL PROPERTY INTERFACE_LINK_LIBRARIES "${gl_imports}")
    target_compile_definitions(eden-headless PRIVATE EDEN_PS5_VULKAN=1)
    if(EDEN_VULKAN_DRIVER STREQUAL "RADV")
        target_compile_definitions(eden-headless PRIVATE EDEN_PS5_RADV=1)
        include("${EDEN_PORT_DIR}/nir_backend.cmake")
    endif()
    target_include_directories(eden-headless BEFORE PRIVATE "${PORT_BUILD_DIR}/include")
endif()

option(EDEN_DEV_VULKAN "Force Vulkan for a development autoboot without changing preferences" OFF)
if(EDEN_DEV_VULKAN)
    if(NOT EDEN_PS5_VULKAN OR EDEN_DEV_ROM_ID STREQUAL "")
        message(FATAL_ERROR "EDEN_DEV_VULKAN requires Vulkan and an explicit development ROM ID")
    endif()
    target_compile_definitions(eden-headless PRIVATE EDEN_DEV_VULKAN=1)
endif()

# Temporary MKW sampler: retain the conversion caller chain without rebuilding every core file.
if(PS5_NATIVE)
    set_property(SOURCE "${PROJECT_SOURCE_DIR}/src/video_core/textures/decoders.cpp"
        "${PROJECT_SOURCE_DIR}/src/video_core/texture_cache/util.cpp"
        TARGET_DIRECTORY video_core APPEND PROPERTY COMPILE_OPTIONS
            -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer)
endif()
