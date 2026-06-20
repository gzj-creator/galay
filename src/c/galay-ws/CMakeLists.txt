add_library(galay-c-ws ws.cc)
add_library(galay::c-ws ALIAS galay-c-ws)

target_compile_features(galay-c-ws PRIVATE cxx_std_23)
target_include_directories(galay-c-ws
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-c-ws
    PUBLIC
        galay::c-common
    PRIVATE
        galay::ws
)

set_target_properties(galay-c-ws PROPERTIES EXPORT_NAME c-ws)

