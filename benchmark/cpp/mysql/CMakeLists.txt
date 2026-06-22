function(galay_add_mysql_benchmark target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::mysql)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
endfunction()

file(GLOB GALAY_MYSQL_BENCHMARK_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/b*.cc"
)
foreach(benchmark_source IN LISTS GALAY_MYSQL_BENCHMARK_SOURCES)
    get_filename_component(benchmark_name "${benchmark_source}" NAME_WE)
    if(benchmark_name STREQUAL "b1_pressure")
        set(target_name benchmark_mysql_sync_query_pressure)
    elseif(benchmark_name STREQUAL "b2_pressure")
        set(target_name benchmark_mysql_async_query_pressure)
    elseif(benchmark_name STREQUAL "b3_pool_pressure")
        set(target_name benchmark_mysql_async_pool_pressure)
    elseif(benchmark_name STREQUAL "b4_pool_lease_pressure")
        set(target_name benchmark_mysql_async_pool_lease_pressure)
    else()
        string(REGEX REPLACE "^b[0-9]+_" "" scenario_name "${benchmark_name}")
        set(target_name benchmark_mysql_${scenario_name})
    endif()
    galay_add_mysql_benchmark(${target_name} ${benchmark_source})
endforeach()
