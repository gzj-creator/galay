function(galay_add_postgres_benchmark target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::postgres)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${target_name} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endfunction()

file(GLOB GALAY_POSTGRES_BENCHMARK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/b*.cc")
foreach(source IN LISTS GALAY_POSTGRES_BENCHMARK_SOURCES)
    get_filename_component(name "${source}" NAME_WE)
    string(REGEX REPLACE "^b[0-9]+_" "" scenario "${name}")
    galay_add_postgres_benchmark(benchmark_postgres_${scenario} ${source})
endforeach()

# External client libraries are historical fixtures only. The formal
# comparison policy uses the Boost.Asio coroutine baseline; no libpq
# competitor target is registered here.
