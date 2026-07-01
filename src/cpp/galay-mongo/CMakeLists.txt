file(GLOB_RECURSE GALAY_MONGO_SOURCES CONFIGURE_DEPENDS "*.cc")

add_library(galay-mongo ${GALAY_MONGO_SOURCES})
add_library(galay::mongo ALIAS galay-mongo)

target_compile_features(galay-mongo PUBLIC cxx_std_23)
target_include_directories(galay-mongo
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-mongo
    PUBLIC
        galay::kernel
        galay::utils
)

if(GALAY_SSL_FEATURE_ENABLED)
    target_link_libraries(galay-mongo PUBLIC galay::ssl)
    target_compile_definitions(galay-mongo PUBLIC GALAY_SSL_FEATURE_ENABLED)
endif()

set_target_properties(galay-mongo PROPERTIES EXPORT_NAME mongo)
