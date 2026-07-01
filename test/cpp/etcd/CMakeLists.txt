if(NOT TARGET PkgConfig::SIMDJSON)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SIMDJSON REQUIRED IMPORTED_TARGET simdjson)
endif()

file(GLOB GALAY_ETCD_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
set(GALAY_ETCD_TEST_SKIP_RETURN_CODE 125)
set(GALAY_ETCD_INTEGRATION_TESTS
    t1_smoke
    t2_prefix
    t3_pipe
    t4_smoke
    t5_pipe
    t8_task_watch
    t13_cluster_integration
    t14_async_cluster_integration
)

function(galay_etcd_configure_test_target target_name)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include)
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
endfunction()

function(galay_etcd_add_test_target source_name)
    get_filename_component(file_name "${source_name}" NAME_WE)
    set(target_name "etcd_${file_name}")
    add_executable(${target_name} ${source_name})
    target_link_libraries(${target_name} PRIVATE galay::etcd)
    if(file_name STREQUAL "t6_helpers")
        target_link_libraries(${target_name} PRIVATE PkgConfig::SIMDJSON)
    endif()
    set_property(TARGET ${target_name} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    galay_etcd_configure_test_target(${target_name})
    string(REGEX REPLACE "_" "." scenario_name "${file_name}")
    add_test(NAME "etcd.${scenario_name}" COMMAND $<TARGET_FILE:${target_name}>)
    if(file_name IN_LIST GALAY_ETCD_INTEGRATION_TESTS)
        set_tests_properties("etcd.${scenario_name}" PROPERTIES
            LABELS "etcd;integration;requires-etcd"
            SKIP_RETURN_CODE ${GALAY_ETCD_TEST_SKIP_RETURN_CODE}
        )
    else()
        set_tests_properties("etcd.${scenario_name}" PROPERTIES LABELS "etcd;unit")
    endif()
endfunction()

foreach(test_source IN LISTS GALAY_ETCD_TEST_SOURCES)
    galay_etcd_add_test_target(${test_source})
endforeach()
