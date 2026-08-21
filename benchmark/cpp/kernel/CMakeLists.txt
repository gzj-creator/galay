function(galay_add_kernel_benchmark target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::kernel)
    target_include_directories(${target_name}
        PRIVATE
            ${PROJECT_SOURCE_DIR}
            ${CMAKE_BINARY_DIR}/include
    )
    set_target_properties(${target_name} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
endfunction()

file(GLOB GALAY_KERNEL_BENCHMARK_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/b*.cc"
)

foreach(benchmark_source IN LISTS GALAY_KERNEL_BENCHMARK_SOURCES)
    get_filename_component(benchmark_name "${benchmark_source}" NAME_WE)
    string(REGEX REPLACE "^b[0-9]+_" "" scenario_name "${benchmark_name}")
    galay_add_kernel_benchmark(benchmark_kernel_${scenario_name} ${benchmark_source})
endforeach()

# The only external kernel baseline is the same-language Boost.Asio coroutine
# harness. Older Crossbeam/libuv sources remain as historical fixtures but are
# intentionally not part of the formal benchmark build.
find_path(GALAY_BOOST_ASIO_INCLUDE_DIR
    NAMES boost/asio.hpp
    HINTS
        "$ENV{BOOST_ROOT}/include"
        /opt/homebrew/include
        /usr/local/include
        /usr/include
)
if(GALAY_BOOST_ASIO_INCLUDE_DIR)
    find_package(Threads REQUIRED)
    add_executable(
        benchmark_kernel_compare_boost_asio_coro_udp
        compare/boost-asio-coro/boost_asio_coro_udp.cc
    )
    add_executable(
        benchmark_kernel_compare_boost_asio_coro_tcp
        compare/boost-asio-coro/boost_asio_coro_tcp.cc
    )
    target_include_directories(benchmark_kernel_compare_boost_asio_coro_udp
        PRIVATE ${GALAY_BOOST_ASIO_INCLUDE_DIR}
    )
    target_include_directories(benchmark_kernel_compare_boost_asio_coro_tcp
        PRIVATE ${GALAY_BOOST_ASIO_INCLUDE_DIR}
    )
    target_compile_definitions(benchmark_kernel_compare_boost_asio_coro_udp
        PRIVATE BOOST_ERROR_CODE_HEADER_ONLY=1 BOOST_SYSTEM_NO_DEPRECATED=1
    )
    target_compile_definitions(benchmark_kernel_compare_boost_asio_coro_tcp
        PRIVATE BOOST_ERROR_CODE_HEADER_ONLY=1 BOOST_SYSTEM_NO_DEPRECATED=1
    )
    target_link_libraries(benchmark_kernel_compare_boost_asio_coro_udp
        PRIVATE Threads::Threads
    )
    target_link_libraries(benchmark_kernel_compare_boost_asio_coro_tcp
        PRIVATE Threads::Threads
    )
    set_target_properties(benchmark_kernel_compare_boost_asio_coro_udp PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    set_target_properties(benchmark_kernel_compare_boost_asio_coro_tcp PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
else()
    message(STATUS
        "Skipping Boost.Asio coroutine benchmarks: boost/asio.hpp not found")
endif()
