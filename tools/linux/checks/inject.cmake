# SPDX-License-Identifier: GPL-3.0-or-later
# Configure via -DCMAKE_PROJECT_yuzu_INCLUDE=/absolute/path/to/this/file
# and -DEGLANTINE_NUP_CHECK_SOURCE=/path/to/tools/linux/checks/nup_check.cpp.
include("${CMAKE_CURRENT_LIST_DIR}/../port-support.cmake")
cmake_language(DEFER CALL add_linux_port_support)

function(add_linux_nup_check)
    if(NOT EXISTS "${EGLANTINE_NUP_CHECK_SOURCE}")
        message(FATAL_ERROR "Set EGLANTINE_NUP_CHECK_SOURCE to the Linux NupContainer check")
    endif()
    # The frontends provide VMA implementation; this standalone core check needs it too.
    file(WRITE "${CMAKE_BINARY_DIR}/linux_nup_vma.cpp"
        "#define VMA_IMPLEMENTATION\n#include \"video_core/vulkan_common/vma.h\"\n")
    add_executable(eglantine-linux-nup-check
        "${EGLANTINE_NUP_CHECK_SOURCE}" "${CMAKE_BINARY_DIR}/linux_nup_vma.cpp")
    target_include_directories(eglantine-linux-nup-check PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_options(eglantine-linux-nup-check PRIVATE -fno-rtti)
    target_link_libraries(eglantine-linux-nup-check PRIVATE
        common core input_common frontend_common video_core ${PLATFORM_LIBRARIES} Threads::Threads)
endfunction()
cmake_language(DEFER CALL add_linux_nup_check)
