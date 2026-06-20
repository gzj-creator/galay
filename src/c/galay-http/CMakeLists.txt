add_library(galay-c-http http.cc)
add_library(galay::c-http ALIAS galay-c-http)

target_compile_features(galay-c-http PUBLIC cxx_std_23)
target_include_directories(galay-c-http
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-c-http
    PUBLIC
        galay::c-common
        galay::http
)

set_target_properties(galay-c-http PROPERTIES EXPORT_NAME c-http)
