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

add_library(rpc common/anchor.cc)
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
