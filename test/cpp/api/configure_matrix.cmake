set(work "${GALAY_BINARY_DIR}/api-configure")
set(source "${work}/source")
file(MAKE_DIRECTORY "${source}/src" "${source}/thirdparty")
file(COPY "${GALAY_SOURCE_DIR}/CMakeLists.txt" DESTINATION "${source}")
file(APPEND "${source}/CMakeLists.txt" "
add_executable(plain_http_consumer \"${GALAY_SOURCE_DIR}/test/cpp/http/t37_native_builder.cc\")
target_link_libraries(plain_http_consumer PRIVATE galay::http)
")
file(COPY "${GALAY_SOURCE_DIR}/cmake" DESTINATION "${source}")
file(CREATE_LINK "${GALAY_SOURCE_DIR}/src/cpp" "${source}/src/cpp" SYMBOLIC RESULT linked)
if(NOT linked STREQUAL "0")
    message(FATAL_ERROR "Cannot create configure fixture: ${linked}")
endif()
file(CREATE_LINK "${GALAY_SOURCE_DIR}/thirdparty/concurrentqueue"
    "${source}/thirdparty/concurrentqueue" SYMBOLIC RESULT linked)
if(NOT linked STREQUAL "0")
    message(FATAL_ERROR "Cannot create concurrentqueue fixture: ${linked}")
endif()

set(flags -G Ninja -DCMAKE_CXX_COMPILER=${GALAY_CXX_COMPILER}
    -DBUILD_TESTING=OFF -DGALAY_BUILD_EXAMPLES=OFF -DGALAY_BUILD_BENCHMARKS=OFF
    -DGALAY_BUILD_C_API=OFF -DGALAY_ENABLE_CPP23_MODULES=OFF
    -DGALAY_INSTALL_CPP23_MODULE_INTERFACES=OFF -DGALAY_DISABLE_IOURING=ON
    -DGALAY_BUILD_UTILS=ON -DGALAY_BUILD_KERNEL=ON -DGALAY_BUILD_HTTP=ON)
foreach(module SSL WS HTTP2 REDIS ETCD MYSQL POSTGRES MONGO RPC MCP TRACING)
    list(APPEND flags -DGALAY_BUILD_${module}=OFF)
endforeach()

function(check_config name expected_result expected_message)
    execute_process(COMMAND "${CMAKE_COMMAND}" -S "${source}" -B "${work}/${name}"
        ${flags} ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    file(WRITE "${work}/${name}.log" "${output}\n${error}")
    if(expected_result STREQUAL "success")
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "${name} failed: ${output}\n${error}")
        endif()
    elseif(result EQUAL 0 OR NOT "${output}${error}" MATCHES "${expected_message}")
        message(FATAL_ERROR "${name}: expected explicit ${expected_message}, got ${result}: ${output}\n${error}")
    endif()
endfunction()

# The fixture deliberately has no serde submodule, yet HTTP without API must configure.
check_config(api_off success "" -DGALAY_BUILD_API=OFF -DGALAY_BUILD_SERDE=OFF)
file(READ "${work}/api_off/build.ninja" off_build)
# Inspect target rules, not absolute fixture paths that may contain galay-api.
if(off_build MATCHES "(^|\n)build (galay-api|serde_simdjson):|(^|\n)build src/cpp/galay-api/|(^|\n)build [^\n]*swagger_ui\\.cc[: ]")
    message(FATAL_ERROR "API=OFF introduced API dependencies")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${work}/api_off"
    --target plain_http_consumer --parallel 2
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
file(WRITE "${work}/api_off_build.log" "${output}\n${error}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Plain HTTP without API/serde failed to build: ${output}\n${error}")
endif()
execute_process(COMMAND "${work}/api_off/plain_http_consumer" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Plain HTTP without API/serde failed to run: ${result}")
endif()
check_config(missing_http failure "GALAY_BUILD_API requires GALAY_BUILD_HTTP=ON"
    -DGALAY_BUILD_API=ON -DGALAY_BUILD_HTTP=OFF -DGALAY_BUILD_SERDE=ON)
check_config(missing_serde_option failure "GALAY_BUILD_API requires GALAY_BUILD_HTTP=ON"
    -DGALAY_BUILD_API=ON -DGALAY_BUILD_SERDE=OFF)
check_config(missing_serde_source failure "Galay serde submodule is missing"
    -DGALAY_BUILD_API=ON -DGALAY_BUILD_SERDE=ON)

# API=ON requires build-time vendored resources, not an installed runtime path.
set(source "${work}/source-with-serde")
file(MAKE_DIRECTORY "${source}/src" "${source}/thirdparty")
file(COPY "${GALAY_SOURCE_DIR}/CMakeLists.txt" DESTINATION "${source}")
file(COPY "${GALAY_SOURCE_DIR}/cmake" DESTINATION "${source}")
foreach(pair IN ITEMS "src/cpp" "thirdparty/concurrentqueue" "thirdparty/serde")
    file(CREATE_LINK "${GALAY_SOURCE_DIR}/${pair}" "${source}/${pair}" SYMBOLIC RESULT linked)
    if(NOT linked STREQUAL "0")
        message(FATAL_ERROR "Cannot create missing-UI configure fixture: ${linked}")
    endif()
endforeach()
check_config(missing_ui_resources failure "galay-api requires vendored Swagger UI resource"
    -DGALAY_BUILD_API=ON -DGALAY_BUILD_SERDE=ON)
message(STATUS "API configure matrix: OFF independent; missing HTTP/serde/build-time UI dependencies explicit")
