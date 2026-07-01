galay_ensure_simdjson()

file(GLOB GALAY_ETCD_SOURCES CONFIGURE_DEPENDS
    "base/*.cc"
    "async/*.cc"
    "cluster/*.cc"
    "sync/*.cc"
)

add_library(galay-etcd ${GALAY_ETCD_SOURCES})
add_library(galay::etcd ALIAS galay-etcd)

target_compile_features(galay-etcd PUBLIC cxx_std_23)
target_include_directories(galay-etcd
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-etcd
    PUBLIC
        galay::kernel
        galay::utils
        galay::http
        simdjson::simdjson
)

set_target_properties(galay-etcd PROPERTIES EXPORT_NAME etcd)
