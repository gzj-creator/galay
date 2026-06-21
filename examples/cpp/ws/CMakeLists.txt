function(galay_add_ws_example target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::ws)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
    if(GALAY_HTTP_COROUTINE_WORKAROUND_ENABLED)
        target_compile_options(${target_name} PRIVATE ${GALAY_HTTP_COROUTINE_WORKAROUND_FLAGS})
    endif()
    set_target_properties(${target_name} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON)
endfunction()

function(galay_add_ws_import_example target_name source_file)
    if(NOT TARGET galay-ws-modules)
        return()
    endif()
    galay_add_ws_example(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay-ws-modules)
    set_target_properties(${target_name} PROPERTIES CXX_EXTENSIONS OFF CXX_SCAN_FOR_MODULES ON)
endfunction()

galay_add_ws_example(example_ws_include_ws_server include/e3_ws_server.cc)
galay_add_ws_example(example_ws_include_ws_client include/e4_ws_client.cc)

if(GALAY_SSL_FEATURE_ENABLED)
    galay_add_ws_example(example_ws_include_wss_server include/e7_wss_server.cc)
    galay_add_ws_example(example_ws_include_wss_client include/e8_wss_client.cc)
endif()

galay_add_ws_import_example(example_ws_import_ws_server import/e3_ws_server.cc)
galay_add_ws_import_example(example_ws_import_ws_client import/e4_ws_client.cc)

if(GALAY_SSL_FEATURE_ENABLED)
    galay_add_ws_import_example(example_ws_import_wss_server import/e7_wss_server.cc)
    galay_add_ws_import_example(example_ws_import_wss_client import/e8_wss_client.cc)
endif()

file(GLOB GALAY_WS_MANUAL_EXAMPLE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/manual/*.cc"
)
foreach(example_source IN LISTS GALAY_WS_MANUAL_EXAMPLE_SOURCES)
    get_filename_component(example_name "${example_source}" NAME_WE)
    if(example_name MATCHES "wss" AND NOT GALAY_SSL_FEATURE_ENABLED)
        continue()
    endif()

    set(example_target "example_ws_manual_${example_name}")
    galay_add_ws_example(${example_target} ${example_source})
    target_include_directories(${example_target} PRIVATE ${PROJECT_SOURCE_DIR})
endforeach()
