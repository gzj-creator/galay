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
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include)
    galay_mongo_configure_test_target(${target_name})
    string(REGEX REPLACE "^t[0-9]+_" "" scenario_name "${target_name}")
    set(test_name "mongo.${scenario_name}")
    add_test(NAME ${test_name} COMMAND $<TARGET_FILE:${target_name}>)
    set_tests_properties(${test_name} PROPERTIES LABELS "mongo;unit")
endfunction()

function(add_mongo_integration_target target_name source_file)
    add_executable(${target_name} EXCLUDE_FROM_ALL ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::mongo)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include)
    galay_mongo_configure_test_target(${target_name})
endfunction()

function(add_mongo_integration_test target_name source_file test_name)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::mongo)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include)
    galay_mongo_configure_test_target(${target_name})
    add_test(NAME ${test_name} COMMAND $<TARGET_FILE:${target_name}>)
    set_tests_properties(${test_name} PROPERTIES LABELS "mongo;integration")
endfunction()

file(GLOB GALAY_MONGO_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

# Integration-only targets: compiled but not registered with ctest (EXCLUDE_FROM_ALL).
set(GALAY_MONGO_INTEGRATION_TARGETS
    t2_client
    t3_pipeline
    t4_func
    t5_func
    t6_auth
    t7_bridge
)

# Integration tests registered with ctest under mongo.it.* namespace.
set(GALAY_MONGO_INTEGRATION_TESTS
    t11_replica_set_discovery_it
)

foreach(test_source IN LISTS GALAY_MONGO_TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)
    if(test_name IN_LIST GALAY_MONGO_INTEGRATION_TARGETS)
        add_mongo_integration_target(mongo_${test_name} ${test_source})
    elseif(test_name IN_LIST GALAY_MONGO_INTEGRATION_TESTS)
        string(REGEX REPLACE "^t[0-9]+_" "" scenario_suffix "${test_name}")
        string(REGEX REPLACE "_it$" "" scenario_suffix "${scenario_suffix}")
        add_mongo_integration_test(mongo_${test_name} ${test_source} "mongo.it.${scenario_suffix}")
    else()
        add_mongo_test(${test_name} ${test_source})
        if(test_name STREQUAL "t9_socket_options_source" OR
           test_name STREQUAL "t13_expected_error_source")
            target_compile_definitions(${test_name}
                PRIVATE GALAY_MONGO_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/cpp"
            )
        endif()
    endif()
endforeach()
