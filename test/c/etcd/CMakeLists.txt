function(galay_add_c_etcd_test source_file)
    get_filename_component(source_name "${source_file}" NAME_WE)
    string(REGEX REPLACE "^t[0-9]+_" "" scenario_name "${source_name}")
    set(target_name "test_c_etcd_${scenario_name}")
    add_executable(${target_name} ${source_file})
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_BINARY_DIR}/include
            ${CMAKE_SOURCE_DIR}/src/c
    )
    target_link_libraries(${target_name} PRIVATE galay::c-etcd galay::c-kernel)
    add_test(NAME "c.etcd.${scenario_name}" COMMAND ${target_name})
    if(scenario_name STREQUAL "header_smoke")
        set_tests_properties("c.etcd.${scenario_name}" PROPERTIES LABELS "c;etcd;surface")
    else()
        set_tests_properties("c.etcd.${scenario_name}" PROPERTIES LABELS "c;etcd;unit")
    endif()
endfunction()

file(GLOB GALAY_C_ETCD_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
foreach(test_source IN LISTS GALAY_C_ETCD_TEST_SOURCES)
    galay_add_c_etcd_test(${test_source})
endforeach()
