set(GALAY_MYSQL_TEST_SKIP_RETURN_CODE 125)

function(add_mysql_test target_name source_file)
    cmake_parse_arguments(ARG "INTEGRATION" "" "LABELS" ${ARGN})
    set(executable_name "mysql_${target_name}")
    add_executable(${executable_name} ${source_file})
    target_link_libraries(${executable_name} PRIVATE galay::mysql)
    target_include_directories(${executable_name} PRIVATE ${CMAKE_SOURCE_DIR})
    set_target_properties(${executable_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    set(test_name "mysql.${target_name}")
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

# t3 - 异步客户端测试
add_mysql_test(t3_client t3_client.cc INTEGRATION)

# t4 - 同步客户端测试
add_mysql_test(t4_client t4_client.cc INTEGRATION)

# t5 - 连接池测试
add_mysql_test(t5_pool t5_pool.cc INTEGRATION)

# t6 - 事务测试
add_mysql_test(t6_transaction t6_transaction.cc INTEGRATION)

# t7 - 预处理语句测试
add_mysql_test(t7_stmt t7_stmt.cc INTEGRATION)

# t8 - Awaitable surface test
add_mysql_test(t8_surface t8_surface.cc)

# t9 - Config env override surface
add_mysql_test(t9_env t9_env.cc)

# t10 - MySQL 库级日志入口测试
add_mysql_test(t10_log t10_log.cc)

add_test(
    NAME PackageConfig.ConsumerSmoke
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
        -P ${CMAKE_SOURCE_DIR}/test/mysql/package/package_consumer_smoke.cmake
)
set_tests_properties(PackageConfig.ConsumerSmoke PROPERTIES
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    LABELS "mysql;package;consumer"
    TIMEOUT 120
)
