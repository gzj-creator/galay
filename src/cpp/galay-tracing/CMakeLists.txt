galay_ensure_openssl()

file(GLOB_RECURSE GALAY_TRACING_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/common/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/context/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/kernel/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/log/*.cc"
)

add_library(galay-tracing ${GALAY_TRACING_SOURCES})
add_library(galay::tracing ALIAS galay-tracing)

target_compile_features(galay-tracing PUBLIC cxx_std_23)
target_include_directories(galay-tracing
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-tracing PUBLIC galay::kernel OpenSSL::Crypto)

if(GALAY_TRACING_ENABLE_GALAY_HTTP_OTLP_TRANSPORT)
    target_compile_definitions(galay-tracing PUBLIC GALAY_TRACING_ENABLE_OTLP_HTTP=1)
    target_link_libraries(galay-tracing PUBLIC galay::http)
endif()

set_target_properties(galay-tracing PROPERTIES EXPORT_NAME tracing)

if(GALAY_TRACING_ENABLE_SPDLOG)
    galay_ensure_spdlog()

    file(GLOB GALAY_TRACING_SPDLOG_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/adapters/*spdlog*.cc")
    add_library(galay-tracing-spdlog ${GALAY_TRACING_SPDLOG_SOURCES})
    add_library(galay::tracing-spdlog ALIAS galay-tracing-spdlog)
    target_compile_features(galay-tracing-spdlog PUBLIC cxx_std_23)
    target_include_directories(galay-tracing-spdlog
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(galay-tracing-spdlog PUBLIC galay::tracing spdlog::spdlog)
    set_target_properties(galay-tracing-spdlog PROPERTIES EXPORT_NAME tracing-spdlog)
endif()

add_library(galay-tracing-kernel INTERFACE)
add_library(galay::tracing-kernel ALIAS galay-tracing-kernel)
target_compile_features(galay-tracing-kernel INTERFACE cxx_std_23)
target_include_directories(galay-tracing-kernel
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-tracing-kernel INTERFACE galay::tracing galay::kernel)
set_target_properties(galay-tracing-kernel PROPERTIES EXPORT_NAME tracing-kernel)
