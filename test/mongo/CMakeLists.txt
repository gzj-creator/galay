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

add_mongo_test(t1_bson t1_bson.cc)
add_mongo_test(t8_builder t8_builder.cc)
add_mongo_test(t9_socket_options_source t9_socket_options_source.cc)
target_compile_definitions(t9_socket_options_source
    PRIVATE GALAY_MONGO_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src"
)
add_mongo_test(t10_log t10_log.cc)
