function(galay_add_mongo_benchmark target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::mongo)
    target_include_directories(${target_name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    galay_mongo_apply_cxx(${target_name})
endfunction()

galay_add_mongo_benchmark(benchmark_mongo_sync_ping_throughput b1_ping.cc)
galay_add_mongo_benchmark(benchmark_mongo_async_ping_throughput b2_ping.cc)
