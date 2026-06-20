add_executable(example_c_etcd_config_builder e1_config_builder.c)
target_include_directories(example_c_etcd_config_builder
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(example_c_etcd_config_builder PRIVATE galay::c-etcd)
