function(galay_etcd_configure_benchmark_target target_name)
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
endfunction()

function(galay_add_etcd_benchmark target_name)
    add_executable(${target_name} ${ARGN})
    target_link_libraries(${target_name} PRIVATE galay::etcd)
    target_include_directories(${target_name} PRIVATE ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR})
    set_property(TARGET ${target_name} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    galay_etcd_configure_benchmark_target(${target_name})
endfunction()

galay_add_etcd_benchmark(benchmark_etcd_sync_kv_throughput b1_kv.cc)
galay_add_etcd_benchmark(benchmark_etcd_async_kv_throughput b2_kv.cc bench_support.cc)
