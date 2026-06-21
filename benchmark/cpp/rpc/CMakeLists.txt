function(galay_add_rpc_benchmark target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::rpc)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include)
    set_target_properties(${target_name} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
endfunction()

file(GLOB GALAY_RPC_BENCHMARK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/b*.cc")

foreach(benchmark_source IN LISTS GALAY_RPC_BENCHMARK_SOURCES)
    get_filename_component(benchmark_name "${benchmark_source}" NAME_WE)
    galay_add_rpc_benchmark("benchmark_rpc_${benchmark_name}" "${benchmark_source}")
endforeach()
