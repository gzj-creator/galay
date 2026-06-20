set(GALAY_MYSQL_TEST_SKIP_RETURN_CODE 125)

function(add_mysql_test target_name source_file)
    cmake_parse_arguments(ARG "INTEGRATION" "" "LABELS" ${ARGN})
    set(executable_name "mysql_${target_name}")
    add_executable(${executable_name} ${source_file})
    target_link_libraries(${executable_name} PRIVATE galay::mysql)
    target_include_directories(${executable_name} PRIVATE ${CMAKE_BINARY_DIR}/include ${CMAKE_SOURCE_DIR})
    set_target_properties(${executable_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    string(REGEX REPLACE "^t[0-9]+_" "" scenario_name "${target_name}")
    set(test_name "mysql.${scenario_name}")
    add_test(NAME ${test_name} COMMAND $<TARGET_FILE:${executable_name}>)

    set(test_labels mysql)
    if(ARG_INTEGRATION)
        list(APPEND test_labels integration requires-mysql)
        set_tests_properties(${test_name} PROPERTIES SKIP_RETURN_CODE ${GALAY_MYSQL_TEST_SKIP_RETURN_CODE})
    else()
        list(APPEND test_labels unit)
    endif()

    if(ARG_LABELS)
        list(APPEND test_labels ${ARG_LABELS})
    endif()

    set_tests_properties(${test_name} PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "${test_labels}"
        TIMEOUT 30
    )
endfunction()

# t0 - 测试配置契约
add_mysql_test(t0_config t0_config.cc)

# t1 - 协议解析测试
add_mysql_test(t1_protocol t1_protocol.cc)

# t2 - 认证测试
add_mysql_test(t2_auth t2_auth.cc)

# t8 - Awaitable surface test
add_mysql_test(t8_surface t8_surface.cc)

# t9 - Config env override surface
add_mysql_test(t9_env t9_env.cc)

# t10 - MySQL 库级日志入口测试
add_mysql_test(t10_log t10_log.cc)

# t11 - Real server authentication plugin matrix
add_mysql_test(t11_auth_plugins t11_auth_plugins.cc INTEGRATION LABELS auth_plugins)

# t12 - Real server reconnect/failure recovery smoke
add_mysql_test(t12_resilience t12_resilience.cc INTEGRATION LABELS resilience)

# t13 - Async pool coroutine source guard
add_mysql_test(t13_pool_coroutine_source t13_pool_coroutine_source.cc)
target_compile_definitions(mysql_t13_pool_coroutine_source
    PRIVATE GALAY_MYSQL_SOURCE_DIR="${CMAKE_SOURCE_DIR}/src/cpp"
)

# t14 - Async pool waiter/resume integration
add_mysql_test(t14_pool_waiter t14_pool_waiter.cc INTEGRATION LABELS pool_waiter)

# t15 - Integration test opt-in gate
add_mysql_test(t15_it_gate t15_it_gate.cc INTEGRATION LABELS it_gate)

# t16 - RAII pool lease integration
add_mysql_test(t16_pool_lease t16_pool_lease.cc INTEGRATION LABELS pool_lease)

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
