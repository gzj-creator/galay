if(TARGET galay::kernel)
    set(_galay_etcd_kernel_target galay::kernel)
elseif(TARGET galay-kernel)
    add_library(galay::kernel ALIAS galay-kernel)
    set(_galay_etcd_kernel_target galay::kernel)
elseif(TARGET kernel)
    add_library(galay::kernel ALIAS kernel)
    set(_galay_etcd_kernel_target galay::kernel)
elseif(TARGET galay-kernel::galay-kernel)
    set(_galay_etcd_kernel_target galay-kernel::galay-kernel)
else()
    message(FATAL_ERROR "etcd requires an in-tree kernel target")
endif()

if(TARGET galay::utils)
    set(_galay_etcd_utils_target galay::utils)
elseif(TARGET galay-utils)
    add_library(galay::utils ALIAS galay-utils)
    set(_galay_etcd_utils_target galay::utils)
elseif(TARGET utils)
    add_library(galay::utils ALIAS utils)
    set(_galay_etcd_utils_target galay::utils)
elseif(TARGET galay::galay-utils)
    set(_galay_etcd_utils_target galay::galay-utils)
else()
    message(FATAL_ERROR "etcd requires an in-tree utils target")
endif()

if(TARGET galay::http)
    set(_galay_etcd_http_target galay::http)
elseif(TARGET http)
    add_library(galay::http ALIAS http)
    set(_galay_etcd_http_target galay::http)
elseif(TARGET galay-http)
    add_library(galay::http ALIAS galay-http)
    set(_galay_etcd_http_target galay::http)
elseif(TARGET galay-http::galay-http)
    set(_galay_etcd_http_target galay-http::galay-http)
else()
    message(FATAL_ERROR "etcd requires an in-tree http target")
endif()

galay_ensure_simdjson()

file(GLOB GALAY_ETCD_SOURCES CONFIGURE_DEPENDS
    "base/*.cc"
    "async/*.cc"
    "cluster/*.cc"
    "sync/*.cc"
)

add_library(etcd ${GALAY_ETCD_SOURCES})
add_library(galay::etcd ALIAS etcd)

if(NOT TARGET galay-etcd)
    add_library(galay-etcd ALIAS etcd)
endif()
if(NOT TARGET galay-etcd::galay-etcd)
    add_library(galay-etcd::galay-etcd ALIAS etcd)
endif()

target_compile_features(etcd PUBLIC cxx_std_23)
target_include_directories(etcd
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(etcd
    PUBLIC
        ${_galay_etcd_kernel_target}
        ${_galay_etcd_utils_target}
        ${_galay_etcd_http_target}
        simdjson::simdjson
)

set_target_properties(etcd PROPERTIES EXPORT_NAME etcd)
