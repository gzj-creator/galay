# Exercise upstream headers and modules through Galay's dependency targets.
function(galay_add_serde_test target_name source_file)
    add_executable("${target_name}" "${source_file}")
    target_link_libraries("${target_name}" PRIVATE galay::serde)
    set_target_properties("${target_name}" PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
    )
    string(REGEX REPLACE "^t[0-9]+_" "" scenario_name "${target_name}")
    add_test(NAME "serde.${scenario_name}" COMMAND "${target_name}")
    set_tests_properties("serde.${scenario_name}" PROPERTIES LABELS "serde;unit")
endfunction()

file(GLOB GALAY_SERDE_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/t*.cc")

foreach(test_source IN LISTS GALAY_SERDE_TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)
    if(test_name STREQUAL "t12_shared_backend" AND
       (NOT TARGET galay-mcp OR NOT TARGET galay-etcd))
        continue()
    endif()
    if(test_name STREQUAL "t10_import_smoke" AND
       NOT GALAY_SERDE_CPP23_MODULES_EFFECTIVE)
        continue()
    endif()
    galay_add_serde_test("${test_name}" "${test_source}")
    if(test_name STREQUAL "t14_contract" OR test_name STREQUAL "t18_wire_fields")
        if(MSVC)
            target_compile_options("${test_name}" PRIVATE /UNDEBUG)
        else()
            target_compile_options("${test_name}" PRIVATE -UNDEBUG)
        endif()
    endif()
    if(test_name STREQUAL "t12_shared_backend")
        target_link_libraries("${test_name}" PRIVATE galay::mcp galay::etcd)
    endif()
    if(test_name STREQUAL "t10_import_smoke")
        set_target_properties("${test_name}" PROPERTIES
            CXX_STANDARD 23
            CXX_SCAN_FOR_MODULES ON
        )
    endif()
endforeach()

add_test(NAME serde.install_consumer
    COMMAND ${CMAKE_COMMAND}
        "-DGALAY_BINARY_DIR=${CMAKE_BINARY_DIR}"
        "-DGALAY_GENERATOR=${CMAKE_GENERATOR}"
        "-DGALAY_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DGALAY_COMPILER_FLAGS=${CMAKE_CXX_FLAGS}"
        "-DGALAY_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
        "-DGALAY_CONFIG=$<CONFIG>"
        "-DGALAY_EXPECT_NO_MODULES=$<NOT:$<BOOL:${GALAY_INSTALL_CPP23_MODULE_INTERFACES}>>"
        "-DGALAY_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/install_consumer.cmake")
set_tests_properties(serde.install_consumer PROPERTIES
    LABELS "serde;package" TIMEOUT 180 RESOURCE_LOCK galay_build_install)
