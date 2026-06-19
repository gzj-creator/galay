function(galay_mongo_configure_test_target target_name)
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
endfunction()

function(add_mongo_test target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::mongo)
    target_include_directories(${target_name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    galay_mongo_configure_test_target(${target_name})
    string(REGEX REPLACE "^t[0-9]+_" "" scenario_name "${target_name}")
    set(test_name "mongo.${scenario_name}")
    add_test(NAME ${test_name} COMMAND $<TARGET_FILE:${target_name}>)
    set_tests_properties(${test_name} PROPERTIES LABELS "mongo;unit")
endfunction()

function(add_mongo_integration_target target_name source_file)
    add_executable(${target_name} EXCLUDE_FROM_ALL ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::mongo)
    target_include_directories(${target_name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    galay_mongo_configure_test_target(${target_name})
endfunction()

function(add_mongo_integration_test target_name source_file test_name)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::mongo)
    target_include_directories(${target_name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    galay_mongo_configure_test_target(${target_name})
    add_test(NAME ${test_name} COMMAND $<TARGET_FILE:${target_name}>)
    set_tests_properties(${test_name} PROPERTIES LABELS "mongo;integration")
endfunction()

add_mongo_test(t1_bson t1_bson.cc)
add_mongo_test(t8_builder t8_builder.cc)
add_mongo_test(t9_socket_options_source t9_socket_options_source.cc)
target_compile_definitions(t9_socket_options_source
    PRIVATE GALAY_MONGO_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src"
)
add_mongo_test(t10_log t10_log.cc)
add_mongo_test(t12_uri_unit t12_uri_unit.cc)

add_mongo_integration_target(mongo_t2_client t2_client.cc)
add_mongo_integration_target(mongo_t3_pipeline t3_pipeline.cc)
add_mongo_integration_target(mongo_t4_func t4_func.cc)
add_mongo_integration_target(mongo_t5_func t5_func.cc)
add_mongo_integration_target(mongo_t6_auth t6_auth.cc)
add_mongo_integration_target(mongo_t7_bridge t7_bridge.cc)

add_mongo_integration_test(mongo_t11_replica_set_discovery_it
    t11_replica_set_discovery_it.cc
    mongo.it.replica_set_discovery
)
