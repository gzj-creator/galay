file(GLOB GALAY_RPC_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/common/*.cc")
add_library(galay-rpc ${GALAY_RPC_SOURCES})
add_library(galay::rpc ALIAS galay-rpc)

target_compile_features(galay-rpc PUBLIC cxx_std_23)
target_include_directories(galay-rpc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-rpc
    PUBLIC
        galay::kernel
        galay::utils
)

if(GALAY_RPC_ENABLE_ETCD)
    if(NOT TARGET galay::etcd)
        message(FATAL_ERROR "GALAY_RPC_ENABLE_ETCD requires the canonical target galay::etcd")
    endif()
    target_compile_definitions(galay-rpc PUBLIC GALAY_RPC_HAS_ETCD=1)
    target_link_libraries(galay-rpc PUBLIC galay::etcd)
endif()

set_target_properties(galay-rpc PROPERTIES EXPORT_NAME rpc)

set(GALAY_RPC_CPP23_MODULES_EFFECTIVE OFF)
if(GALAY_ENABLE_CPP23_MODULES)
    if(CMAKE_VERSION VERSION_LESS 3.28)
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON requires CMake >= 3.28 for galay-rpc; skipping RPC module file set")
    elseif(NOT CMAKE_GENERATOR MATCHES "Ninja|Visual Studio")
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON requires a C++ module-capable generator for galay-rpc; skipping RPC module file set")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON is not enabled for AppleClang in this project; skipping RPC module file set")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON requires Clang >= 16 for galay-rpc; skipping RPC module file set")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 14)
        message(WARNING "GALAY_ENABLE_CPP23_MODULES=ON requires GCC >= 14 for galay-rpc; skipping RPC module file set")
    else()
        set(GALAY_RPC_CPP23_MODULES_EFFECTIVE ON)
    endif()
endif()

if(GALAY_RPC_CPP23_MODULES_EFFECTIVE)
    file(GLOB GALAY_RPC_MODULE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/module/galay_rpc.cppm")
    if(GALAY_RPC_ENABLE_ETCD)
        list(APPEND GALAY_RPC_MODULE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/module/galay_rpc_etcd.cppm")
    endif()
    target_include_directories(galay-rpc
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    target_sources(galay-rpc
        PUBLIC
            FILE_SET CXX_MODULES
            BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/module
            FILES ${GALAY_RPC_MODULE_SOURCES}
    )
    set_target_properties(galay-rpc PROPERTIES CXX_SCAN_FOR_MODULES ON)
endif()

set(GALAY_RPC_CPP23_MODULES_EFFECTIVE "${GALAY_RPC_CPP23_MODULES_EFFECTIVE}" PARENT_SCOPE)
