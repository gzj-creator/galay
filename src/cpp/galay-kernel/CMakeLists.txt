file(GLOB_RECURSE KERNEL_SOURCES CONFIGURE_DEPENDS
    "*.cc"
    "*.h"
    "*.hpp"
    "*.inl"
)

add_library(galay-kernel ${KERNEL_SOURCES})
add_library(galay::kernel ALIAS galay-kernel)
target_compile_features(galay-kernel PUBLIC cxx_std_23)

galay_configure_io_backend(GALAY_KERNEL_BACKEND PLATFORM_LIBS)

target_include_directories(galay-kernel
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(galay-kernel
    PUBLIC
        galay::utils
        ${PLATFORM_LIBS}
)

if(GALAY_KERNEL_BACKEND STREQUAL "epoll")
    target_compile_definitions(galay-kernel PUBLIC USE_EPOLL)
elseif(GALAY_KERNEL_BACKEND STREQUAL "io_uring")
    target_compile_definitions(galay-kernel PUBLIC USE_IOURING)
elseif(GALAY_KERNEL_BACKEND STREQUAL "kqueue")
    target_compile_definitions(galay-kernel PUBLIC USE_KQUEUE)
else()
    message(FATAL_ERROR "GALAY_KERNEL_BACKEND is not set correctly: ${GALAY_KERNEL_BACKEND}")
endif()

set_target_properties(galay-kernel PROPERTIES EXPORT_NAME kernel)

set(GALAY_KERNEL_CPP23_MODULES_EFFECTIVE OFF)
if(GALAY_ENABLE_CPP23_MODULES)
    if(CMAKE_VERSION VERSION_LESS 3.28)
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON requires CMake >= 3.28 for galay-kernel; skipping kernel module file set")
    elseif(NOT CMAKE_GENERATOR MATCHES "Ninja|Visual Studio")
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON requires a C++ module-capable generator for galay-kernel; skipping kernel module file set")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON is not enabled for AppleClang in this project; skipping kernel module file set")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON requires Clang >= 16 for galay-kernel; skipping kernel module file set")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON requires GCC >= 14 for galay-kernel; skipping kernel module file set")
    else()
        set(GALAY_KERNEL_CPP23_MODULES_EFFECTIVE ON)
    endif()
endif()

if(GALAY_KERNEL_CPP23_MODULES_EFFECTIVE)
    file(GLOB GALAY_KERNEL_MODULE_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/module/galay_kernel.cppm"
    )
    if(GALAY_INSTALL_CPP23_MODULE_INTERFACES)
        target_sources(galay-kernel
            PUBLIC
                FILE_SET CXX_MODULES
                BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/module
                FILES ${GALAY_KERNEL_MODULE_SOURCES}
        )
        set_target_properties(galay-kernel PROPERTIES CXX_SCAN_FOR_MODULES ON)
    else()
        # Keep the module dependency visible to in-tree consumers without
        # exporting a file set whose source interface is not installed.
        add_library(galay-kernel-cpp23-modules OBJECT)
        set_target_properties(galay-kernel-cpp23-modules PROPERTIES
            POSITION_INDEPENDENT_CODE ${GALAY_BUILD_SHARED_LIBS}
        )
        target_compile_features(galay-kernel-cpp23-modules PUBLIC cxx_std_23)
        target_include_directories(galay-kernel-cpp23-modules
            PUBLIC
                $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        )
        target_link_libraries(galay-kernel-cpp23-modules
            PUBLIC
                galay::utils
                ${PLATFORM_LIBS}
        )
        if(GALAY_KERNEL_BACKEND STREQUAL "epoll")
            target_compile_definitions(galay-kernel-cpp23-modules PUBLIC USE_EPOLL)
        elseif(GALAY_KERNEL_BACKEND STREQUAL "io_uring")
            target_compile_definitions(galay-kernel-cpp23-modules PUBLIC USE_IOURING)
        elseif(GALAY_KERNEL_BACKEND STREQUAL "kqueue")
            target_compile_definitions(galay-kernel-cpp23-modules PUBLIC USE_KQUEUE)
        endif()
        target_sources(galay-kernel-cpp23-modules
            PUBLIC
                FILE_SET CXX_MODULES
                BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/module
                FILES ${GALAY_KERNEL_MODULE_SOURCES}
        )
        set_target_properties(galay-kernel-cpp23-modules PROPERTIES CXX_SCAN_FOR_MODULES ON)
        target_link_libraries(galay-kernel PUBLIC
            "$<BUILD_INTERFACE:galay-kernel-cpp23-modules>"
        )
    endif()
endif()

set(GALAY_KERNEL_CPP23_MODULES_EFFECTIVE "${GALAY_KERNEL_CPP23_MODULES_EFFECTIVE}" PARENT_SCOPE)
