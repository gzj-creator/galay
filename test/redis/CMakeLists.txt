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
    string(REGEX REPLACE "_" "." SCENARIO_NAME "${EXECUTABLE_NAME}")
    add_test(NAME "redis.${SCENARIO_NAME}" COMMAND ${EXECUTABLE_NAME})
    if(EXECUTABLE_NAME STREQUAL "t13_cluster")
        set_tests_properties("redis.${SCENARIO_NAME}" PROPERTIES LABELS "redis;integration")
    elseif(EXECUTABLE_NAME IN_LIST GALAY_REDIS_UNIT_TESTS)
        set_tests_properties("redis.${SCENARIO_NAME}" PROPERTIES LABELS "unit;redis")
    else()
        set_tests_properties("redis.${SCENARIO_NAME}" PROPERTIES LABELS redis)
    endif()
endforeach()
