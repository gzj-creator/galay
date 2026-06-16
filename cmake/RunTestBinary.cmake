if(NOT DEFINED TEST_BINARY_DIR)
    message(FATAL_ERROR "TEST_BINARY_DIR is required")
endif()

if(NOT DEFINED TEST_BINARY_NAME)
    message(FATAL_ERROR "TEST_BINARY_NAME is required")
endif()

set(_galay_test_binary "${TEST_BINARY_DIR}/${TEST_BINARY_NAME}")
if(NOT EXISTS "${_galay_test_binary}")
    message(FATAL_ERROR "Test binary does not exist: ${_galay_test_binary}")
endif()

if(DEFINED TEST_WORKING_DIRECTORY)
    set(_galay_test_working_directory "${TEST_WORKING_DIRECTORY}")
else()
    set(_galay_test_working_directory "${TEST_BINARY_DIR}")
endif()

execute_process(
    COMMAND "${_galay_test_binary}"
    WORKING_DIRECTORY "${_galay_test_working_directory}"
    RESULT_VARIABLE _galay_test_result
)

if(NOT _galay_test_result EQUAL 0)
    message(FATAL_ERROR "Test binary failed with exit code ${_galay_test_result}: ${_galay_test_binary}")
endif()
