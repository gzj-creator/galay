function(galay_add_c_kernel_example target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::c-kernel)
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_BINARY_DIR}/include
            ${CMAKE_SOURCE_DIR}/src/c
    )
    set_target_properties(${target_name} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
endfunction()

galay_add_c_kernel_example(example_c_kernel_runtime e1_runtime.c)
