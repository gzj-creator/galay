add_executable(test_c_http_header_smoke t1_header_smoke.c)
target_include_directories(test_c_http_header_smoke
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(test_c_http_header_smoke PRIVATE galay::c-http)
add_test(NAME c.http.header_smoke COMMAND test_c_http_header_smoke)
set_tests_properties(c.http.header_smoke PROPERTIES LABELS "c;http;surface")

add_executable(test_c_http_request_response_surface t2_request_response_surface.c)
target_include_directories(test_c_http_request_response_surface
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(test_c_http_request_response_surface PRIVATE galay::c-http)
add_test(NAME c.http.request_response_surface COMMAND test_c_http_request_response_surface)
set_tests_properties(c.http.request_response_surface PROPERTIES LABELS "c;http;unit")
