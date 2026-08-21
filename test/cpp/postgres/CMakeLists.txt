set(GALAY_POSTGRES_TEST_SKIP_RETURN_CODE 125)
file(GLOB GALAY_POSTGRES_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

set(GALAY_POSTGRES_INTEGRATION_TESTS
    t5_client
    t7_prepared
    t8_transaction
    t10_pool
    t12_pipeline
    t13_sync_client
)

function(add_postgres_test target_name source_file)
    set(executable_name "postgres_${target_name}")
    add_executable(${executable_name} ${source_file})
    target_link_libraries(${executable_name} PRIVATE galay::postgres)
    target_include_directories(${executable_name} PRIVATE ${CMAKE_BINARY_DIR}/include ${CMAKE_SOURCE_DIR})
    set_target_properties(${executable_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    string(REGEX REPLACE "_" "." scenario_name "${target_name}")
    set(test_name "postgres.${scenario_name}")
    add_test(NAME ${test_name} COMMAND $<TARGET_FILE:${executable_name}>)

    set(test_labels postgres)
    set(test_timeout 30)
    if(target_name IN_LIST GALAY_POSTGRES_INTEGRATION_TESTS)
        list(APPEND test_labels integration requires-postgres)
        set_tests_properties(${test_name} PROPERTIES
            SKIP_RETURN_CODE ${GALAY_POSTGRES_TEST_SKIP_RETURN_CODE}
            RESOURCE_LOCK galay_postgres_integration
        )
        set(test_timeout 60)
    else()
        list(APPEND test_labels unit)
    endif()
    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "${test_labels}"
        TIMEOUT ${test_timeout}
    )
endfunction()

foreach(test_source IN LISTS GALAY_POSTGRES_TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)
    add_postgres_test(${test_name} ${test_source})
endforeach()
