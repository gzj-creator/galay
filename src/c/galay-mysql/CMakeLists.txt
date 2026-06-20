add_library(galay-c-mysql mysql.cc)
add_library(galay::c-mysql ALIAS galay-c-mysql)

target_compile_features(galay-c-mysql PUBLIC cxx_std_23)
target_include_directories(galay-c-mysql
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-c-mysql
    PUBLIC
        galay::c-common
        galay::mysql
)

set_target_properties(galay-c-mysql PROPERTIES EXPORT_NAME c-mysql)
