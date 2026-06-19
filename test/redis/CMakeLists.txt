file(GLOB SRC_LIST CONFIGURE_DEPENDS *.cc)

set(GALAY_REDIS_UNIT_TESTS
    t3_protocol
    t8_batch
    t9_span
    t10_command
    t12_topology
    t14_size
    t15_surface
    t20_encoder
    t26_log
)

set(GALAY_REDIS_TLS_TESTS
    t16_rediss
    t17_tls
    t18_url
    t19_topology
)

if(GALAY_BUILD_SSL)
    list(APPEND GALAY_REDIS_UNIT_TESTS t16_rediss)
endif()

foreach(SRC ${SRC_LIST})
    get_filename_component(FILE_NAME ${SRC} NAME_WE)
    if(NOT FILE_NAME IN_LIST GALAY_REDIS_UNIT_TESTS)
        continue()
    endif()
    if(FILE_NAME IN_LIST GALAY_REDIS_TLS_TESTS AND NOT GALAY_BUILD_SSL)
        continue()
    endif()
    set(EXECUTABLE_NAME ${FILE_NAME})
    add_executable(${EXECUTABLE_NAME} ${SRC})
    set_property(TARGET ${EXECUTABLE_NAME} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    target_link_libraries(${EXECUTABLE_NAME} PRIVATE galay::redis)
    target_include_directories(${EXECUTABLE_NAME} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    set_target_properties(${EXECUTABLE_NAME} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        INSTALL_RPATH $ORIGIN/../lib
    )
    string(REGEX REPLACE "^t[0-9]+_" "" SCENARIO_NAME "${EXECUTABLE_NAME}")
    add_test(NAME "redis.${SCENARIO_NAME}" COMMAND ${EXECUTABLE_NAME})
    set_tests_properties("redis.${SCENARIO_NAME}" PROPERTIES LABELS "unit;redis")
endforeach()

function(add_redis_integration_target target_name source_file test_name)
    add_executable(${target_name} EXCLUDE_FROM_ALL ${source_file})
    set_property(TARGET ${target_name} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    target_link_libraries(${target_name} PRIVATE galay::redis)
    target_include_directories(${target_name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        INSTALL_RPATH $ORIGIN/../lib
    )
    add_test(NAME "${test_name}" COMMAND ${target_name})
    set_tests_properties("${test_name}" PROPERTIES LABELS "redis;integration")
endfunction()

add_redis_integration_target(redis_t13_cluster t13_cluster.cc redis.t13.cluster)
add_redis_integration_target(redis_t27_auth t27_auth.cc redis.t27.auth)

if(GALAY_BUILD_SSL)
    add_redis_integration_target(redis_t17_tls t17_tls.cc redis.t17.tls)
    add_redis_integration_target(redis_t18_url t18_url.cc redis.t18.url)
    add_redis_integration_target(redis_t19_topology t19_topology.cc redis.t19.topology)
endif()
