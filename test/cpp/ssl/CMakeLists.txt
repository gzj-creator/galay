file(GLOB GALAY_SSL_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

function(add_ssl_test source_file)
    get_filename_component(target_name "${source_file}" NAME_WE)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::ssl)
    set_property(TARGET ${target_name} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    target_include_directories(${target_name} BEFORE PRIVATE ${CMAKE_BINARY_DIR}/include)
    string(REGEX REPLACE "_" "." scenario_name "${target_name}")
    set(test_name "ssl.${scenario_name}")
    add_test(NAME ${test_name} COMMAND ${target_name})
    set_tests_properties(${test_name} PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endfunction()

foreach(test_source IN LISTS GALAY_SSL_TEST_SOURCES)
    add_ssl_test(${test_source})
    get_filename_component(test_name "${test_source}" NAME_WE)
    if(test_name STREQUAL "t13_timeout")
        target_compile_options(t13_timeout PRIVATE -Werror=switch)
    endif()
endforeach()

# 复制测试证书到 bin 目录和 build 根目录
set(_galay_ssl_cert_destinations
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)
if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    list(APPEND _galay_ssl_cert_destinations ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
endif()
list(REMOVE_DUPLICATES _galay_ssl_cert_destinations)
foreach(_galay_ssl_cert_destination IN LISTS _galay_ssl_cert_destinations)
    file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/certs
         DESTINATION ${_galay_ssl_cert_destination})
endforeach()
