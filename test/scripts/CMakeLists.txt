add_test(
    NAME scripts.linux_exec_matrix
    COMMAND python3 -B "${CMAKE_CURRENT_SOURCE_DIR}/t1_linux_exec_matrix.py"
)
set_tests_properties(scripts.linux_exec_matrix PROPERTIES LABELS "scripts;unit")
