add_library(galay-c-rpc rpc.cc)
add_library(galay::c-rpc ALIAS galay-c-rpc)

target_compile_features(galay-c-rpc PRIVATE cxx_std_23)
target_include_directories(galay-c-rpc
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-c-rpc
    PUBLIC
        galay::c-common
    PRIVATE
        galay::rpc
)

set_target_properties(galay-c-rpc PROPERTIES EXPORT_NAME c-rpc)
