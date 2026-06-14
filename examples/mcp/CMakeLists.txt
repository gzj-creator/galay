function(galay_add_mcp_example target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::mcp)
    target_include_directories(${target_name} PRIVATE ${PROJECT_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endfunction()

function(galay_add_mcp_import_example target_name source_file)
    if(NOT TARGET galay-mcp-modules)
        return()
    endif()
    galay_add_mcp_example(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay-mcp-modules)
    set_target_properties(${target_name} PROPERTIES CXX_SCAN_FOR_MODULES ON)
endfunction()

galay_add_mcp_example(example_mcp_include_stdio include/e1_stdio.cc)
galay_add_mcp_example(example_mcp_include_http include/e2_http.cc)

galay_add_mcp_import_example(example_mcp_import_stdio import/e1_stdio.cc)
galay_add_mcp_import_example(example_mcp_import_http import/e2_http.cc)
