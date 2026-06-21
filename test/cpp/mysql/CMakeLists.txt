set(GALAY_MYSQL_TEST_SKIP_RETURN_CODE 125)
file(GLOB GALAY_MYSQL_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

set(GALAY_MYSQL_INTEGRATION_TESTS
    t4_client
    t5_client
    t6_pool
    t7_transaction
    t8_stmt
    t12_auth_plugins
    t13_resilience
    t15_pool_waiter
    t16_it_gate
    t17_pool_lease
)

function(add_mysql_test target_name source_file)
    set(executable_name "mysql_${target_name}")
    add_executable(${executable_name} ${source_file})
    target_link_libraries(${executable_name} PRIVATE galay::mysql)
    target_include_directories(${executable_name} PRIVATE ${CMAKE_BINARY_DIR}/include ${CMAKE_SOURCE_DIR})
    set_target_properties(${executable_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    string(REGEX REPLACE "_" "." scenario_name "${target_name}")
    set(test_name "mysql.${scenario_name}")
    add_test(NAME ${test_name} COMMAND $<TARGET_FILE:${executable_name}>)

    set(test_labels mysql)
    if(target_name IN_LIST GALAY_MYSQL_INTEGRATION_TESTS)
        list(APPEND test_labels integration requires-mysql)
        set_tests_properties(${test_name} PROPERTIES SKIP_RETURN_CODE ${GALAY_MYSQL_TEST_SKIP_RETURN_CODE})
    else()
        list(APPEND test_labels unit)
    endif()

    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "${test_labels}"
        TIMEOUT 30
    )
endfunction()

foreach(test_source IN LISTS GALAY_MYSQL_TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)
    add_mysql_test(${test_name} ${test_source})
    if(test_name STREQUAL "t14_pool_coroutine_source")
        target_compile_definitions(mysql_t14_pool_coroutine_source
            PRIVATE GALAY_MYSQL_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/cpp"
        )
    endif()
endforeach()

add_test(
    NAME mysql.package_consumer_smoke
    COMMAND
        ${CMAKE_COMMAND}
        -DGALAY_MYSQL_SOURCE_DIR=${CMAKE_SOURCE_DIR}
        -DGALAY_MYSQL_BINARY_DIR=${CMAKE_BINARY_DIR}
        -DGALAY_MYSQL_CMAKE_GENERATOR=${CMAKE_GENERATOR}
        -DGALAY_MYSQL_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DGALAY_MYSQL_CXX_STANDARD=23
        -DGALAY_MYSQL_PACKAGE_CMAKE_PREFIX_PATH=${CMAKE_PREFIX_PATH}
        -DGALAY_MYSQL_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DGALAY_MYSQL_INSTALL_CONFIG=$<CONFIG>
        -P ${CMAKE_SOURCE_DIR}/test/cpp/mysql/package/package_consumer_smoke.cmake
)
set_tests_properties(mysql.package_consumer_smoke PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    LABELS "mysql;package;consumer"
    TIMEOUT 120
)
