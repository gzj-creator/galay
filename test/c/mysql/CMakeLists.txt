add_executable(test_c_mysql_header_smoke t1_header_smoke.c)
target_include_directories(test_c_mysql_header_smoke
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(test_c_mysql_header_smoke PRIVATE galay::c-mysql)
add_test(NAME c.mysql.header_smoke COMMAND test_c_mysql_header_smoke)
set_tests_properties(c.mysql.header_smoke PROPERTIES LABELS "c;mysql;surface")

add_executable(test_c_mysql_surface t2_mysql_surface.c)
target_include_directories(test_c_mysql_surface
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(test_c_mysql_surface PRIVATE galay::c-mysql)
add_test(NAME c.mysql.surface COMMAND test_c_mysql_surface)
set_tests_properties(c.mysql.surface PROPERTIES LABELS "c;mysql;unit")
