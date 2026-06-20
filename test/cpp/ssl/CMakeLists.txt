function(add_ssl_test target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::ssl)
    set_property(TARGET ${target_name} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    target_include_directories(${target_name} BEFORE PRIVATE ${CMAKE_BINARY_DIR}/include)
    string(REGEX REPLACE "^t[0-9]+_" "" scenario_name "${target_name}")
    set(test_name "ssl.${scenario_name}")
    add_test(NAME ${test_name} COMMAND ${target_name})
    set_tests_properties(${test_name} PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endfunction()

add_ssl_test(t1_socket t1_socket.cc)
add_ssl_test(t2_loopback t2_loopback.cc)
add_ssl_test(t3_policy t3_policy.cc)
add_ssl_test(t4_state t4_state.cc)
add_ssl_test(t5_io t5_io.cc)
add_ssl_test(t6_custom t6_custom.cc)
add_ssl_test(t7_builder t7_builder.cc)
add_ssl_test(t8_proto t8_proto.cc)
add_ssl_test(t9_bridge t9_bridge.cc)
add_ssl_test(t10_resume t10_resume.cc)
add_ssl_test(t12_fanout t12_fanout.cc)
add_ssl_test(t13_handshake t13_handshake.cc)
add_ssl_test(t14_timeout t14_timeout.cc)
target_compile_options(t14_timeout PRIVATE -Werror=switch)

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
