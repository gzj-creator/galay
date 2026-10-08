set(work "${TEST_BINARY_DIR}/startup-certificates")
file(MAKE_DIRECTORY "${work}")
if(TEST_WITH_SSL)
    find_program(openssl openssl REQUIRED)
    execute_process(COMMAND "${openssl}" req -x509 -newkey rsa:2048 -nodes -days 2
        -subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1
        -out "${work}/localhost.crt" -keyout "${work}/localhost.key"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Create local test certificate: ${output}\n${error}")
    endif()
    execute_process(COMMAND "${openssl}" genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048
        -out "${work}/other.key"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Create mismatched test key: ${output}\n${error}")
    endif()
    file(WRITE "${work}/invalid.pem" "invalid certificate or private key\n")
endif()
execute_process(COMMAND "${TEST_BINARY}" "${work}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "API startup acceptance failed: ${result}")
endif()
