file(GLOB GALAY_CONFIG_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/t*.cc")
foreach(test_source IN LISTS GALAY_CONFIG_TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)
    add_executable(${test_name} ${test_source})
    target_include_directories(${test_name} PRIVATE ${CMAKE_BINARY_DIR}/include)
    string(REGEX REPLACE "^t_module_config_" "module_" scenario_name "${test_name}")
    string(REGEX REPLACE "^t_" "" scenario_name "${scenario_name}")
    add_test(NAME "config.${scenario_name}" COMMAND ${test_name})
endforeach()

add_test(
    NAME config.install_include_layout
    COMMAND ${CMAKE_COMMAND}
        -D GALAY_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -D GALAY_BINARY_DIR=${CMAKE_BINARY_DIR}
        -D GALAY_CMAKE_GENERATOR=${CMAKE_GENERATOR}
        -D GALAY_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -D GALAY_INSTALL_CONFIG=$<CONFIG>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/install_include_layout.cmake
)

add_test(
    NAME config.tracing_options_surface
    COMMAND ${CMAKE_COMMAND}
        -D GALAY_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -D GALAY_BINARY_DIR=${CMAKE_BINARY_DIR}
        -D GALAY_CMAKE_GENERATOR=${CMAKE_GENERATOR}
        -D GALAY_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tracing_options_surface.cmake
)
