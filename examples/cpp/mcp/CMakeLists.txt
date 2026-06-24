function(galay_add_mcp_example target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::mcp)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
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

file(GLOB GALAY_MCP_INCLUDE_EXAMPLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.cc")
foreach(example_source IN LISTS GALAY_MCP_INCLUDE_EXAMPLE_SOURCES)
    get_filename_component(example_name "${example_source}" NAME_WE)
    string(REGEX REPLACE "^e[0-9]+_" "" scenario_name "${example_name}")
    galay_add_mcp_example(example_mcp_include_${scenario_name} ${example_source})
endforeach()

file(GLOB GALAY_MCP_IMPORT_EXAMPLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/import/*.cc")
foreach(example_source IN LISTS GALAY_MCP_IMPORT_EXAMPLE_SOURCES)
    get_filename_component(example_name "${example_source}" NAME_WE)
    string(REGEX REPLACE "^e[0-9]+_" "" scenario_name "${example_name}")
    galay_add_mcp_import_example(example_mcp_import_${scenario_name} ${example_source})
endforeach()
