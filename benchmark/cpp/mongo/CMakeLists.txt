function(galay_mongo_configure_benchmark_target target_name)
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
endfunction()

function(galay_add_mongo_benchmark target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::mongo)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include)
    galay_mongo_configure_benchmark_target(${target_name})
endfunction()

set(GALAY_MONGO_BENCHMARK_SCENARIO_b1_ping sync_ping_throughput)
set(GALAY_MONGO_BENCHMARK_SCENARIO_b2_ping async_ping_throughput)

file(GLOB GALAY_MONGO_BENCHMARK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/b*.cc")
foreach(benchmark_source IN LISTS GALAY_MONGO_BENCHMARK_SOURCES)
    get_filename_component(benchmark_name "${benchmark_source}" NAME_WE)
    string(REGEX REPLACE "^b[0-9]+_" "" scenario_name "${benchmark_name}")
    set(scenario_override "GALAY_MONGO_BENCHMARK_SCENARIO_${benchmark_name}")
    if(DEFINED "${scenario_override}")
        set(scenario_name "${${scenario_override}}")
    endif()
    galay_add_mongo_benchmark(benchmark_mongo_${scenario_name} ${benchmark_source})
endforeach()
