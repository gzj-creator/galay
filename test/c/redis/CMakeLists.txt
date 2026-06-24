function(galay_add_c_redis_test source_file)
    get_filename_component(source_name "${source_file}" NAME_WE)
    string(REGEX REPLACE "^t[0-9]+_" "" scenario_name "${source_name}")
    set(target_name "test_c_redis_${scenario_name}")
    add_executable(${target_name} ${source_file})
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_BINARY_DIR}/include
            ${CMAKE_SOURCE_DIR}/src/c
    )
    target_link_libraries(${target_name} PRIVATE galay::c-redis)
    add_test(NAME "c.redis.${scenario_name}" COMMAND ${target_name})
    if(scenario_name STREQUAL "header_smoke")
        set_tests_properties("c.redis.${scenario_name}" PROPERTIES LABELS "c;redis;surface")
    else()
        set_tests_properties("c.redis.${scenario_name}" PROPERTIES LABELS "c;redis;unit")
    endif()
endfunction()

file(GLOB GALAY_C_REDIS_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
foreach(test_source IN LISTS GALAY_C_REDIS_TEST_SOURCES)
    galay_add_c_redis_test(${test_source})
endforeach()
