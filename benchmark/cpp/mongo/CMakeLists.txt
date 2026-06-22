function(galay_mongo_configure_benchmark_target target_name)
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
endfunction()

function(galay_add_mongo_benchmark target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::mongo)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include)
    galay_mongo_configure_benchmark_target(${target_name})
endfunction()

galay_add_mongo_benchmark(benchmark_mongo_sync_ping_throughput b1_ping.cc)
galay_add_mongo_benchmark(benchmark_mongo_async_ping_throughput b2_ping.cc)
galay_add_mongo_benchmark(benchmark_mongo_expected_error_boundaries b3_expected_error_boundaries.cc)
