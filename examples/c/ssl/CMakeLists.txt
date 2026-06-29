function(galay_add_c_ssl_example target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::c-ssl)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include)
    target_compile_definitions(${target_name}
        PRIVATE
            GALAY_SSL_EXAMPLE_CERT_DIR="${CMAKE_CURRENT_LIST_DIR}/../../../test/cpp/ssl/certs"
    )
    set_target_properties(${target_name} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
endfunction()

file(GLOB GALAY_C_SSL_EXAMPLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
foreach(example_source IN LISTS GALAY_C_SSL_EXAMPLE_SOURCES)
    get_filename_component(source_name "${example_source}" NAME_WE)
    string(REGEX REPLACE "^e[0-9]+_" "" scenario_name "${source_name}")
    galay_add_c_ssl_example("example_c_ssl_${scenario_name}" "${example_source}")
endforeach()
