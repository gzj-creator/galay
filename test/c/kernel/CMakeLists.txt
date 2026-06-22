function(galay_add_c_kernel_test target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::c-kernel)
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_BINARY_DIR}/include
            ${CMAKE_SOURCE_DIR}/src/c
    )
    set_target_properties(${target_name} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
endfunction()

galay_add_c_kernel_test(test_c_kernel_header_smoke t1_header_smoke.c)
add_test(NAME c.kernel.header_smoke COMMAND test_c_kernel_header_smoke)

galay_add_c_kernel_test(test_c_kernel_runtime_lifecycle t2_runtime_lifecycle.c)
add_test(NAME c.kernel.runtime_lifecycle COMMAND test_c_kernel_runtime_lifecycle)

galay_add_c_kernel_test(test_c_kernel_config_and_socket_handles t3_config_and_socket_handles.c)
add_test(NAME c.kernel.config_and_socket_handles COMMAND test_c_kernel_config_and_socket_handles)

galay_add_c_kernel_test(test_c_kernel_tcp_accept_api t4_tcp_accept_api.c)
add_test(NAME c.kernel.tcp_accept_api COMMAND test_c_kernel_tcp_accept_api)
