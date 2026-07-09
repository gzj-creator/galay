function(galay_etcd_configure_benchmark_target target_name)
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
endfunction()

function(galay_add_etcd_benchmark target_name)
    add_executable(${target_name} ${ARGN})
    target_link_libraries(${target_name} PRIVATE galay::etcd)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include ${PROJECT_SOURCE_DIR})
    set_property(TARGET ${target_name} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    galay_etcd_configure_benchmark_target(${target_name})
endfunction()

file(GLOB GALAY_ETCD_BENCHMARK_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/b*.cc"
)
file(GLOB GALAY_ETCD_BENCHMARK_SUPPORT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/bench_support.cc"
)
list(FILTER GALAY_ETCD_BENCHMARK_SOURCES EXCLUDE REGEX "/bench_support\\.cc$")
foreach(benchmark_source IN LISTS GALAY_ETCD_BENCHMARK_SOURCES)
    get_filename_component(benchmark_name "${benchmark_source}" NAME_WE)
    set(extra_sources)
    if(benchmark_name STREQUAL "b2_kv" OR benchmark_name STREQUAL "b3_async_smoke")
        list(APPEND extra_sources ${GALAY_ETCD_BENCHMARK_SUPPORT_SOURCES})
    endif()
    galay_add_etcd_benchmark(benchmark_etcd_${benchmark_name} ${benchmark_source} ${extra_sources})
endforeach()
