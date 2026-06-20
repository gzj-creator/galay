function(galay_add_c_utils_benchmark target_name source_file)
    add_executable("${target_name}" "${source_file}")
    target_link_libraries("${target_name}" PRIVATE galay-c-utils)
    target_include_directories("${target_name}" PRIVATE ${CMAKE_BINARY_DIR}/include)
    set_target_properties("${target_name}" PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
endfunction()

galay_add_c_utils_benchmark(benchmark_c_utils_codec_hash_smoke b1_codec_hash_smoke.c)
