file(GLOB GALAY_REDIS_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
set(GALAY_REDIS_TEST_SKIP_RETURN_CODE 125)

set(GALAY_REDIS_TLS_TESTS
    t11_rediss
    t12_tls
    t13_url
    t14_topology
    t25_rediss_pool_acquire_connected
)

set(GALAY_REDIS_INTEGRATION_TESTS
    t6_pubsub
    t8_cluster
    t17_fastpath
    t18_pipeline
    t20_auth
    t24_pool_waiter_blackbox
)

if(GALAY_BUILD_SSL)
    list(APPEND GALAY_REDIS_INTEGRATION_TESTS
        t12_tls
        t13_url
        t14_topology
    )
endif()

function(add_redis_test source_file)
    get_filename_component(target_name "${source_file}" NAME_WE)
    if(target_name IN_LIST GALAY_REDIS_TLS_TESTS AND NOT GALAY_BUILD_SSL)
        return()
    endif()

    add_executable(${target_name} ${source_file})
    set_property(TARGET ${target_name} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    target_link_libraries(${target_name} PRIVATE galay::redis)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include)
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        INSTALL_RPATH $ORIGIN/../lib
    )

    string(REGEX REPLACE "_" "." scenario_name "${target_name}")
    set(test_name "redis.${scenario_name}")
    add_test(NAME "${test_name}" COMMAND ${target_name})

    if(target_name IN_LIST GALAY_REDIS_INTEGRATION_TESTS)
        set_tests_properties("${test_name}" PROPERTIES
            LABELS "redis;integration;requires-redis"
            SKIP_RETURN_CODE ${GALAY_REDIS_TEST_SKIP_RETURN_CODE}
        )
    else()
        set_tests_properties("${test_name}" PROPERTIES LABELS "redis;unit")
    endif()
endfunction()

foreach(test_source IN LISTS GALAY_REDIS_TEST_SOURCES)
    add_redis_test(${test_source})
endforeach()
