function(galay_add_redis_benchmark target_name source_file)
    add_executable(${target_name} ${source_file})
    set_property(TARGET ${target_name} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    target_link_libraries(${target_name} PRIVATE galay::redis)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include ${PROJECT_SOURCE_DIR}/examples/cpp/redis)
    set_target_properties(${target_name} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endfunction()

file(GLOB GALAY_REDIS_BENCHMARK_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/b*.cc"
)
foreach(benchmark_source IN LISTS GALAY_REDIS_BENCHMARK_SOURCES)
    get_filename_component(benchmark_name "${benchmark_source}" NAME_WE)
    if(benchmark_name MATCHES "tls" AND NOT GALAY_BUILD_SSL)
        continue()
    endif()
    string(REGEX REPLACE "^b[0-9]+_" "" scenario_name "${benchmark_name}")
    galay_add_redis_benchmark(benchmark_redis_${scenario_name} ${benchmark_source})
endforeach()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(GALAY_BENCH_HIREDIS QUIET IMPORTED_TARGET hiredis)
endif()

if(TARGET PkgConfig::GALAY_BENCH_HIREDIS)
    find_package(Threads REQUIRED)
    file(GLOB GALAY_HIREDIS_BENCHMARK_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/compare/hiredis*.cc"
    )
    add_executable(benchmark_redis_hiredis_client_throughput ${GALAY_HIREDIS_BENCHMARK_SOURCES})
    target_link_libraries(benchmark_redis_hiredis_client_throughput
        PRIVATE
            PkgConfig::GALAY_BENCH_HIREDIS
            Threads::Threads
    )
    set_target_properties(benchmark_redis_hiredis_client_throughput PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
else()
    message(STATUS "Skipping benchmark_redis_hiredis_client_throughput: hiredis not found")
endif()
