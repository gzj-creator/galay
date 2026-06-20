add_executable(t_module_config_surface t_module_config_surface.cc)
target_include_directories(t_module_config_surface PRIVATE ${CMAKE_BINARY_DIR}/include)
add_test(NAME config.module_surface COMMAND t_module_config_surface)

add_test(
    NAME config.install_include_layout
    COMMAND ${CMAKE_COMMAND}
        -D GALAY_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -D GALAY_BINARY_DIR=${CMAKE_BINARY_DIR}
        -D GALAY_CMAKE_GENERATOR=${CMAKE_GENERATOR}
        -D GALAY_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -D GALAY_INSTALL_CONFIG=$<CONFIG>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/install_include_layout.cmake
)
