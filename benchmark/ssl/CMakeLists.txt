function(galay_add_ssl_benchmark target_name)
    add_executable(${target_name} ${ARGN})
    target_link_libraries(${target_name} PRIVATE galay::ssl)
    target_include_directories(${target_name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    set_target_properties(${target_name} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
endfunction()

galay_add_ssl_benchmark(benchmark_ssl_tls_client_throughput b1_tls_client_throughput.cc ssl_stats.cc)
galay_add_ssl_benchmark(benchmark_ssl_tls_server_throughput b2_tls_server_throughput.cc)
