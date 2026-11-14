# SPDX-License-Identifier: GPL-3.0-or-later
# Add our frontend after the core's real targets exist.
set(EDEN_PORT_DIR "${CMAKE_CURRENT_LIST_DIR}")
# Guest-memory tests use recoverable page faults; Catch must not replace their signal handler.
if(UNIX AND NOT PS5_NATIVE)
    set(CATCH_CONFIG_NO_POSIX_SIGNALS ON CACHE BOOL "Guest-memory tests use recoverable page faults" FORCE)
endif()
function(add_eden_headless)
    include("${EDEN_PORT_DIR}/CMakeLists.txt")
endfunction()
cmake_language(DEFER CALL add_eden_headless)
# The core (Eglantine/, part of this repository) includes the port's own headers as "headless/..."
# and "src/...": the repository's root is on every include path, after the core's own folders.
get_filename_component(EDEN_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include_directories(AFTER "${EDEN_REPO_ROOT}")
# Build switches that change Eglantine/ itself are compile definitions of every target, set here
# before any target exists, so that every translation unit sees the same declarations:
#   EDEN_DEV_PROFILE        development build: autoboot, timed input replay, profiling counters
#   EDEN_MKW_DIAGNOSTICS    Mario Kart development controls and capture instrumentation
#   EDEN_INLINE_EXCLUSIVES  inline page-table exclusive accesses in the JIT (default ON)
#   EDEN_A32_FASTMEM        A32 fastmem window with JIT fault recovery (default ON)
# The sources test them with #if defined(...); a PS5 build is the one with __PROSPERO__.
option(EDEN_INLINE_EXCLUSIVES "Inline page-table exclusive accesses" ON)
option(EDEN_A32_FASTMEM "A32 fastmem window with JIT fault recovery" ON)
foreach(eden_switch EDEN_DEV_PROFILE EDEN_MKW_DIAGNOSTICS EDEN_INLINE_EXCLUSIVES EDEN_A32_FASTMEM)
    if(${eden_switch})
        add_compile_definitions(${eden_switch}=1)
    endif()
endforeach()
# The program names its sources in log lines and assertions. In a console build they are written
# relative to the trees they come from (src/..., build/..., headless/...), not with the folders of
# the machine that built it: a package must not carry those. Before any target exists, so it holds
# for every one.
if(PS5_NATIVE)
    get_filename_component(EDEN_PORT_ROOT "${EDEN_PORT_DIR}/.." ABSOLUTE)
    add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}/="
                        "-ffile-prefix-map=${CMAKE_BINARY_DIR}/=build/"
                        "-ffile-prefix-map=${EDEN_PORT_ROOT}/=")
endif()
