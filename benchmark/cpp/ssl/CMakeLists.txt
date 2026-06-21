function(galay_add_ssl_benchmark target_name)
    add_executable(${target_name} ${ARGN})
    target_link_libraries(${target_name} PRIVATE galay::ssl)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include)
    set_target_properties(${target_name} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
endfunction()

file(GLOB GALAY_SSL_BENCHMARK_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/b*.cc"
)
foreach(benchmark_source IN LISTS GALAY_SSL_BENCHMARK_SOURCES)
    get_filename_component(benchmark_name "${benchmark_source}" NAME_WE)
    set(extra_sources)
    if(benchmark_name STREQUAL "b1_tls_client_throughput")
        list(APPEND extra_sources ssl_stats.cc)
    endif()
    string(REGEX REPLACE "^b[0-9]+_" "" scenario_name "${benchmark_name}")
    galay_add_ssl_benchmark(benchmark_ssl_${scenario_name} ${benchmark_source} ${extra_sources})
endforeach()
