# SPDX-License-Identifier: GPL-3.0-or-later
# Include from the desktop CMAKE_PROJECT_yuzu_INCLUDE hook; defer this function
# until the emulator targets exist. Keep SDL audio and the desktop renderers.
function(add_linux_port_support)
    get_filename_component(PORT_ROOT "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.." ABSOLUTE)
    set(EDEN_PORT_DIR "${PORT_ROOT}/headless")
    set(PORT_BUILD_DIR "${CMAKE_BINARY_DIR}/linux-port")
    file(MAKE_DIRECTORY "${PORT_BUILD_DIR}")

    # Core public headers name headless/... and src/... headers from this repository.
    target_include_directories(common PUBLIC "${PORT_ROOT}")
    target_include_directories(dynarmic PRIVATE "${PORT_ROOT}")
    foreach(target core network qt_common)
        if(TARGET ${target})
            target_include_directories(${target} PRIVATE "${EDEN_PORT_DIR}")
        endif()
    endforeach()
    # Public Vulkan headers include the port's performance counters.
    target_include_directories(video_core PUBLIC "${EDEN_PORT_DIR}")
    target_link_libraries(core PRIVATE zstd::zstd)
    target_link_libraries(video_core PUBLIC OpenSSL::Crypto)

    get_target_property(common_sources common SOURCES)
    # Keep the desktop's shared backing and virtual fastmem arena. The port's
    # sparse table and JIT allocators are still required by the folded core.
    list(REMOVE_ITEM common_sources sparse_large_vector.cpp)
    set_property(TARGET common PROPERTY SOURCES "${common_sources}")
    target_sources(common PRIVATE "${PORT_ROOT}/src/memory_pages.cpp")

    # Folded BlockOfCode uses the allocator's distinct writable/executable views.
    target_include_directories(dynarmic BEFORE PUBLIC "${PORT_BUILD_DIR}/include")
    include("${EDEN_PORT_DIR}/jit-alias.cmake")

    # The shared present pass includes the port HUD shaders even when the desktop
    # frontend does not enable that overlay.
    execute_process(COMMAND python3 "${PORT_ROOT}/tools/build-hud-shaders.py" "${PORT_BUILD_DIR}"
        COMMAND_ERROR_IS_FATAL ANY)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${PORT_ROOT}/tools/build-hud-shaders.py"
        "${EDEN_PORT_DIR}/vulkan_hud.vert" "${EDEN_PORT_DIR}/vulkan_hud.frag")
    target_include_directories(video_core PRIVATE "${PORT_BUILD_DIR}")

    target_sources(video_core PRIVATE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/port-support.cpp")
endfunction()

# jit-alias.cmake uses the same timestamp-preserving helper as the host checks.
if(NOT COMMAND write_derived)
    function(write_derived path content)
        file(WRITE "${path}.in" "${content}")
        configure_file("${path}.in" "${path}" COPYONLY)
    endfunction()
endif()
