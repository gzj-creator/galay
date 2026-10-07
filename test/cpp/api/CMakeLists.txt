file(GLOB GALAY_API_TEST_SOURCES CONFIGURE_DEPENDS "t*.cc")
foreach(source IN LISTS GALAY_API_TEST_SOURCES)
    get_filename_component(name "${source}" NAME_WE)
    add_executable("api_${name}" "${source}")
    target_link_libraries("api_${name}" PRIVATE galay::api)
    target_compile_definitions("api_${name}" PRIVATE
        GALAY_API_SWAGGER_UI_DIR="${PROJECT_SOURCE_DIR}/assets/swagger-ui")
    string(REGEX REPLACE "^t[0-9]+_" "" scenario "${name}")
    add_test(NAME "api.${scenario}" COMMAND "api_${name}")
    set_tests_properties("api.${scenario}" PROPERTIES LABELS "api;unit" TIMEOUT 60)
endforeach()

add_test(NAME api.embedded_generator COMMAND ${CMAKE_COMMAND}
    -DGALAY_SOURCE_DIR=${PROJECT_SOURCE_DIR}
    -DGALAY_BINARY_DIR=${CMAKE_BINARY_DIR}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/embedded_generator.cmake)
set_tests_properties(api.embedded_generator PROPERTIES LABELS "api;config" TIMEOUT 120)

add_test(NAME api.configure_matrix COMMAND ${CMAKE_COMMAND}
    -DGALAY_SOURCE_DIR=${PROJECT_SOURCE_DIR}
    -DGALAY_BINARY_DIR=${CMAKE_BINARY_DIR}
    -DGALAY_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/configure_matrix.cmake)
set_tests_properties(api.configure_matrix PROPERTIES LABELS "api;config" TIMEOUT 120)

add_test(NAME api.install_consumer COMMAND ${CMAKE_COMMAND}
    -DGALAY_SOURCE_DIR=${PROJECT_SOURCE_DIR}
    -DGALAY_BINARY_DIR=${CMAKE_BINARY_DIR}
    -DGALAY_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/install_consumer.cmake)
set_tests_properties(api.install_consumer PROPERTIES
    LABELS "api;integration;install" TIMEOUT 300 RESOURCE_LOCK galay_build_install)
