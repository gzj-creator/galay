function(add_ssl_test target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay-ssl)
    add_test(NAME ${target_name} COMMAND ${target_name})
    set_tests_properties(${target_name} PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
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
add_ssl_test(t11_steady t11_steady.cc)
add_ssl_test(t12_fanout t12_fanout.cc)
add_ssl_test(t13_handshake t13_handshake.cc)
add_ssl_test(t14_timeout t14_timeout.cc)
target_compile_options(t14_timeout PRIVATE -Werror=switch)

# 复制测试证书到 bin 目录和 build 根目录
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/certs
     DESTINATION ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})
file(COPY ${CMAKE_CURRENT_SOURCE_DIR}/certs
     DESTINATION ${CMAKE_BINARY_DIR})
