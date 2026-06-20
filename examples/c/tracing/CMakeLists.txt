function(galay_add_c_tracing_example target_name source_file)
    add_executable("${target_name}" "${source_file}")
    target_link_libraries("${target_name}" PRIVATE galay::c-tracing)
    target_include_directories("${target_name}" PRIVATE ${CMAKE_BINARY_DIR}/include)
    set_target_properties("${target_name}" PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
endfunction()

galay_add_c_tracing_example(example_c_tracing_traceparent e1_traceparent.c)
