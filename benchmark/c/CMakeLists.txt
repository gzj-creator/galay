if(TARGET galay::c-kernel AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/kernel/CMakeLists.txt")
    add_subdirectory(kernel)
endif()
