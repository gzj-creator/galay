add_executable(example_c_mysql_config_packet e1_config_packet.c)
target_include_directories(example_c_mysql_config_packet
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(example_c_mysql_config_packet PRIVATE galay::c-mysql)
