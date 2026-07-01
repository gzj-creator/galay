file(GLOB_RECURSE GALAY_MYSQL_SOURCES CONFIGURE_DEPENDS "*.cc")

add_library(galay-mysql ${GALAY_MYSQL_SOURCES})
add_library(galay::mysql ALIAS galay-mysql)

target_compile_features(galay-mysql PUBLIC cxx_std_23)
target_include_directories(galay-mysql
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-mysql
    PUBLIC
        galay::kernel
        galay::utils
)

if(GALAY_SSL_FEATURE_ENABLED)
    target_link_libraries(galay-mysql PUBLIC galay::ssl)
    target_compile_definitions(galay-mysql PUBLIC GALAY_SSL_FEATURE_ENABLED)
endif()

set_target_properties(galay-mysql PROPERTIES EXPORT_NAME mysql)
