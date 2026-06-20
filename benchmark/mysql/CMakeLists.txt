function(galay_add_mysql_benchmark target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::mysql)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
endfunction()

galay_add_mysql_benchmark(benchmark_mysql_sync_query_pressure b1_pressure.cc)
galay_add_mysql_benchmark(benchmark_mysql_async_query_pressure b2_pressure.cc)
galay_add_mysql_benchmark(benchmark_mysql_async_pool_pressure b3_pool_pressure.cc)
galay_add_mysql_benchmark(benchmark_mysql_async_pool_lease_pressure b4_pool_lease_pressure.cc)
