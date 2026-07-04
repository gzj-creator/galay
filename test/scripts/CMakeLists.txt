add_test(
    NAME scripts.linux_exec_matrix
    COMMAND python3 -B "${CMAKE_CURRENT_SOURCE_DIR}/t1_linux_exec_matrix.py"
)
set_tests_properties(scripts.linux_exec_matrix PROPERTIES LABELS "scripts;unit")

add_test(
    NAME scripts.http2_h2load_compare
    COMMAND python3 -B "${CMAKE_CURRENT_SOURCE_DIR}/t2_http2_h2load_compare.py"
)
set_tests_properties(scripts.http2_h2load_compare PROPERTIES LABELS "scripts;unit")
