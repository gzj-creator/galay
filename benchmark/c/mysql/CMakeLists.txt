add_executable(benchmark_c_mysql_packet_codec_smoke b1_packet_codec_smoke.c)
target_include_directories(benchmark_c_mysql_packet_codec_smoke
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(benchmark_c_mysql_packet_codec_smoke PRIVATE galay::c-mysql)
