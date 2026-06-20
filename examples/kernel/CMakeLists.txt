function(galay_add_kernel_example target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::kernel)
    target_include_directories(${target_name}
        PRIVATE
            ${PROJECT_SOURCE_DIR}
            ${CMAKE_BINARY_DIR}/include
    )
    set_target_properties(${target_name} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
endfunction()

function(galay_add_kernel_import_example target_name source_file)
    if(NOT TARGET galay-kernel-modules)
        return()
    endif()
    galay_add_kernel_example(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay-kernel-modules)
    set_target_properties(${target_name} PROPERTIES CXX_SCAN_FOR_MODULES ON)
endfunction()

galay_add_kernel_example(example_kernel_include_sendfile include/e1_sendfile.cc)
galay_add_kernel_example(example_kernel_include_tcp_echo_server include/e2_echo.cc)
galay_add_kernel_example(example_kernel_include_tcp_client include/e3_tcp.cc)
galay_add_kernel_example(example_kernel_include_task_basic include/e4_task.cc)
galay_add_kernel_example(example_kernel_include_udp_echo include/e5_udp.cc)
galay_add_kernel_example(example_kernel_include_custom_awaitable include/e10_await.cc)
galay_add_kernel_example(example_kernel_include_builder_protocol include/e11_proto.cc)

galay_add_kernel_import_example(example_kernel_import_sendfile import/e1_sendfile.cc)
galay_add_kernel_import_example(example_kernel_import_tcp_echo_server import/e2_echo.cc)
galay_add_kernel_import_example(example_kernel_import_tcp_client import/e3_tcp.cc)
galay_add_kernel_import_example(example_kernel_import_task_basic import/e4_task.cc)
galay_add_kernel_import_example(example_kernel_import_udp_echo import/e5_udp.cc)
galay_add_kernel_import_example(example_kernel_import_mpsc_channel import/e6_mpsc.cc)
galay_add_kernel_import_example(example_kernel_import_unsafe_channel import/e7_unsafe.cc)
galay_add_kernel_import_example(example_kernel_import_async_sync import/e8_async.cc)
galay_add_kernel_import_example(example_kernel_import_timer_sleep import/e9_sleep.cc)
galay_add_kernel_import_example(example_kernel_import_custom_awaitable import/e10_await.cc)
galay_add_kernel_import_example(example_kernel_import_builder_protocol import/e11_proto.cc)
