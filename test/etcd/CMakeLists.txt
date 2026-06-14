if(NOT TARGET PkgConfig::SIMDJSON)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SIMDJSON REQUIRED IMPORTED_TARGET simdjson)
endif()

function(galay_etcd_add_test_target target_name source_name)
    add_executable(${target_name} ${source_name})
    target_link_libraries(${target_name} PRIVATE galay-etcd)
    set_property(TARGET ${target_name} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    galay_etcd_apply_cxx(${target_name})
    add_test(NAME ${target_name} COMMAND $<TARGET_FILE:${target_name}>)
endfunction()

galay_etcd_add_test_target(T1-EtcdSmoke t1_smoke.cc)
galay_etcd_add_test_target(T2-EtcdPrefixOps t2_prefix.cc)
galay_etcd_add_test_target(T3-EtcdPipeline t3_pipe.cc)
galay_etcd_add_test_target(T4-AsyncEtcdSmoke t4_smoke.cc)
galay_etcd_add_test_target(T5-AsyncEtcdPipeline t5_pipe.cc)

add_executable(T6-EtcdInternalHelpers t6_helpers.cc)
target_link_libraries(T6-EtcdInternalHelpers PRIVATE galay-etcd)
target_link_libraries(T6-EtcdInternalHelpers PRIVATE PkgConfig::SIMDJSON)
set_property(TARGET T6-EtcdInternalHelpers PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
galay_etcd_apply_cxx(T6-EtcdInternalHelpers)
add_test(NAME T6-EtcdInternalHelpers COMMAND $<TARGET_FILE:T6-EtcdInternalHelpers>)

galay_etcd_add_test_target(T7-EtcdAwaitableSurface t7_await.cc)

add_executable(T8-AsyncBenchmarkSmoke
    t8_bench.cc
    ${PROJECT_SOURCE_DIR}/benchmark/etcd/bench_support.cc
)
target_include_directories(T8-AsyncBenchmarkSmoke PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(T8-AsyncBenchmarkSmoke PRIVATE galay-etcd)
set_property(TARGET T8-AsyncBenchmarkSmoke PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
galay_etcd_apply_cxx(T8-AsyncBenchmarkSmoke)
add_test(NAME T8-AsyncBenchmarkSmoke COMMAND $<TARGET_FILE:T8-AsyncBenchmarkSmoke>)

galay_etcd_add_test_target(T9-AsyncEtcdTaskWatch t9_task_watch.cc)
galay_etcd_add_test_target(T10-EtcdLog t10_log.cc)
