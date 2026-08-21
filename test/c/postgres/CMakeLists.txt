function(galay_add_c_postgres_test source_file)
    get_filename_component(source_name "${source_file}" NAME_WE)
    string(REGEX REPLACE "^t[0-9]+_" "" scenario_name "${source_name}")
    string(REGEX REPLACE "^postgres_" "" scenario_name "${scenario_name}")
    set(target_name "test_c_postgres_${scenario_name}")
    add_executable(${target_name} ${source_file})
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_BINARY_DIR}/include
            ${CMAKE_SOURCE_DIR}/src/c
    )
    target_link_libraries(${target_name} PRIVATE galay::c-postgres)
    add_test(NAME "c.postgres.${scenario_name}" COMMAND ${target_name})
    set(test_timeout 30)
    if(scenario_name STREQUAL "surface")
        set(test_labels "c;postgres;surface")
    elseif(scenario_name STREQUAL "real_instance")
        set(test_labels "c;postgres;integration;requires-postgres")
        set_tests_properties("c.postgres.${scenario_name}" PROPERTIES
            SKIP_RETURN_CODE 125
            RESOURCE_LOCK galay_postgres_integration
        )
        set(test_timeout 60)
    else()
        set(test_labels "c;postgres;unit")
    endif()
    set_tests_properties("c.postgres.${scenario_name}" PROPERTIES
        LABELS "${test_labels}"
        TIMEOUT ${test_timeout}
    )
endfunction()

file(GLOB GALAY_C_POSTGRES_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
galay_filter_unsupported_c_coro_sources(GALAY_C_POSTGRES_TEST_SOURCES)
foreach(test_source IN LISTS GALAY_C_POSTGRES_TEST_SOURCES)
    galay_add_c_postgres_test(${test_source})
endforeach()
