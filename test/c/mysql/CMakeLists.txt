function(galay_add_c_mysql_test source_file)
    get_filename_component(source_name "${source_file}" NAME_WE)
    string(REGEX REPLACE "^t[0-9]+_" "" scenario_name "${source_name}")
    string(REGEX REPLACE "^mysql_" "" scenario_name "${scenario_name}")
    set(target_name "test_c_mysql_${scenario_name}")
    add_executable(${target_name} ${source_file})
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_BINARY_DIR}/include
            ${CMAKE_SOURCE_DIR}/src/c
    )
    target_link_libraries(${target_name} PRIVATE galay::c-mysql)
    add_test(NAME "c.mysql.${scenario_name}" COMMAND ${target_name})
    if(scenario_name STREQUAL "header_smoke")
        set_tests_properties("c.mysql.${scenario_name}" PROPERTIES LABELS "c;mysql;surface")
    else()
        set_tests_properties("c.mysql.${scenario_name}" PROPERTIES LABELS "c;mysql;unit")
    endif()
endfunction()

file(GLOB GALAY_C_MYSQL_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
foreach(test_source IN LISTS GALAY_C_MYSQL_TEST_SOURCES)
    galay_add_c_mysql_test(${test_source})
endforeach()
