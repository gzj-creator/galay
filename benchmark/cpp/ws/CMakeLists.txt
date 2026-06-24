function(galay_add_ws_benchmark target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::ws)
    target_include_directories(${target_name} PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}/include
    )
    if(GALAY_HTTP_COROUTINE_WORKAROUND_ENABLED)
        target_compile_options(${target_name} PRIVATE ${GALAY_HTTP_COROUTINE_WORKAROUND_FLAGS})
    endif()
    set_target_properties(${target_name} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endfunction()

file(GLOB GALAY_WS_BENCHMARK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/b*.cc")
foreach(benchmark_source IN LISTS GALAY_WS_BENCHMARK_SOURCES)
    get_filename_component(benchmark_name "${benchmark_source}" NAME_WE)
    if(benchmark_name MATCHES "wss" AND NOT GALAY_SSL_FEATURE_ENABLED)
        continue()
    endif()
    string(REGEX REPLACE "^b[0-9]+_" "" scenario_name "${benchmark_name}")
    galay_add_ws_benchmark(benchmark_ws_${scenario_name} ${benchmark_source})
endforeach()
