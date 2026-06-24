if(TARGET galay::kernel)
    set(_galay_rpc_kernel_target galay::kernel)
elseif(TARGET galay-kernel)
    add_library(galay::kernel ALIAS galay-kernel)
    set(_galay_rpc_kernel_target galay::kernel)
elseif(TARGET kernel)
    add_library(galay::kernel ALIAS kernel)
    set(_galay_rpc_kernel_target galay::kernel)
elseif(TARGET galay-kernel::galay-kernel)
    set(_galay_rpc_kernel_target galay-kernel::galay-kernel)
else()
    message(FATAL_ERROR "rpc requires an in-tree kernel target")
endif()

if(TARGET galay::utils)
    set(_galay_rpc_utils_target galay::utils)
elseif(TARGET galay-utils)
    add_library(galay::utils ALIAS galay-utils)
    set(_galay_rpc_utils_target galay::utils)
elseif(TARGET utils)
    add_library(galay::utils ALIAS utils)
    set(_galay_rpc_utils_target galay::utils)
elseif(TARGET galay::galay-utils)
    set(_galay_rpc_utils_target galay::galay-utils)
else()
    message(FATAL_ERROR "rpc requires an in-tree utils target")
endif()

file(GLOB GALAY_RPC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/common/*.cc")
add_library(rpc ${GALAY_RPC_SOURCES})
add_library(galay::rpc ALIAS rpc)

if(NOT TARGET galay-rpc)
    add_library(galay-rpc ALIAS rpc)
endif()
if(NOT TARGET galay-rpc::galay-rpc)
    add_library(galay-rpc::galay-rpc ALIAS rpc)
endif()

target_compile_features(rpc PUBLIC cxx_std_23)
target_include_directories(rpc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(rpc
    PUBLIC
        ${_galay_rpc_kernel_target}
        ${_galay_rpc_utils_target}
)

set_target_properties(rpc PROPERTIES EXPORT_NAME rpc)

set(GALAY_RPC_CPP23_MODULES_EFFECTIVE OFF)
if(GALAY_ENABLE_CPP23_MODULES)
    if(CMAKE_VERSION VERSION_LESS 3.28)
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON requires CMake >= 3.28 for galay-rpc-modules; skipping module target")
    elseif(NOT CMAKE_GENERATOR MATCHES "Ninja|Visual Studio")
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON requires a C++ module-capable generator for galay-rpc-modules; skipping module target")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON is not enabled for AppleClang in this project; skipping galay-rpc-modules")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON requires Clang >= 16 for galay-rpc-modules; skipping module target")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON requires GCC >= 14 for galay-rpc-modules; skipping module target")
    else()
        set(GALAY_RPC_CPP23_MODULES_EFFECTIVE ON)
    endif()
endif()

if(GALAY_RPC_CPP23_MODULES_EFFECTIVE)
    file(GLOB GALAY_RPC_MODULE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/module/galay_rpc.cppm")
    add_library(galay-rpc-modules)
    add_library(galay-rpc::galay-rpc-modules ALIAS galay-rpc-modules)
    target_compile_features(galay-rpc-modules PUBLIC cxx_std_23)
    target_link_libraries(galay-rpc-modules PUBLIC rpc)
    target_include_directories(galay-rpc-modules
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    target_sources(galay-rpc-modules
        PUBLIC
            FILE_SET CXX_MODULES
            BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/module
            FILES ${GALAY_RPC_MODULE_SOURCES}
    )
    set_target_properties(galay-rpc-modules PROPERTIES
        EXPORT_NAME galay-rpc-modules
        CXX_SCAN_FOR_MODULES ON
    )
endif()
