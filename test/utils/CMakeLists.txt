function(galay_add_utils_test target_name source_file)
    add_executable("${target_name}" "${source_file}")
    target_link_libraries("${target_name}" PRIVATE galay::utils)
    if(target_name STREQUAL "t6_buffer_queue_ring" OR
       target_name STREQUAL "t7_cache_lru" OR
       target_name STREQUAL "t8_concurrency_pool_waiter" OR
       target_name STREQUAL "t12_resilience_rate_circuit")
        target_compile_definitions("${target_name}" PRIVATE
            GALAY_UTILS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/.."
        )
    endif()
    string(REGEX REPLACE "^t[0-9]+_" "" scenario_name "${target_name}")
    string(REGEX REPLACE "_test$" "" scenario_name "${scenario_name}")
    add_test(NAME "utils.${scenario_name}" COMMAND "${target_name}")
    set_tests_properties("utils.${scenario_name}" PROPERTIES LABELS "utils;unit")
endfunction()

set(GALAY_UTILS_TEST_GROUPS
    t4_algorithm_codecs_crypto.cc
    t5_app_cli_config.cc
    t6_buffer_queue_ring.cc
    t7_cache_lru.cc
    t8_concurrency_pool_waiter.cc
    t9_core_string_time_random.cc
    t10_data_structures.cc
    t11_platform_process_system.cc
    t12_resilience_rate_circuit.cc
    t13_routing_balancer_hash.cc
)

foreach(test_source IN LISTS GALAY_UTILS_TEST_GROUPS)
    get_filename_component(test_name "${test_source}" NAME_WE)
    galay_add_utils_test("${test_name}" "${test_source}")
endforeach()

galay_add_utils_test(t1_ring_buffer_iovec t1_ring_buffer_iovec.cc)
galay_add_utils_test(t2_sha1_base64 t2_sha1_base64.cc)
galay_add_utils_test(t3_pbkdf2_hmac_sha256 t3_pbkdf2_hmac_sha256.cc)

if(GALAY_ENABLE_CPP23_MODULES AND TARGET galay-utils-modules)
    galay_add_utils_test(t_import_smoke t_import_smoke.cc)
    target_link_libraries(t_import_smoke PRIVATE galay-utils-modules)
    set_target_properties(t_import_smoke PROPERTIES CXX_SCAN_FOR_MODULES ON)
    set_tests_properties(utils.import_smoke PROPERTIES LABELS "utils;surface")
endif()
