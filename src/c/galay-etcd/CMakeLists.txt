add_library(galay-c-etcd etcd.cc)
add_library(galay::c-etcd ALIAS galay-c-etcd)

target_compile_features(galay-c-etcd PUBLIC cxx_std_23)
target_include_directories(galay-c-etcd
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-c-etcd
    PUBLIC
        galay::c-common
        galay::etcd
)

set_target_properties(galay-c-etcd PROPERTIES EXPORT_NAME c-etcd)
