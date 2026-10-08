if(NOT IS_DIRECTORY "${GALAY_SOURCE_DIR}/src/cpp/galay-api" OR NOT GALAY_BINARY_DIR)
    message(FATAL_ERROR "GALAY_SOURCE_DIR and GALAY_BINARY_DIR are required")
endif()
set(work "${GALAY_BINARY_DIR}/api-transport-matrix")
file(MAKE_DIRECTORY "${work}")

set(flags -G Ninja -DCMAKE_CXX_COMPILER=${GALAY_CXX_COMPILER}
    -DCMAKE_C_COMPILER=${GALAY_C_COMPILER}
    -DCMAKE_CXX_FLAGS=-Werror -DCMAKE_C_FLAGS=-Werror
    -DCMAKE_BUILD_TYPE=Release -DGALAY_BUILD_DEBUG=OFF
    -DBUILD_TESTING=ON -DGALAY_BUILD_EXAMPLES=OFF -DGALAY_BUILD_BENCHMARKS=OFF
    -DGALAY_BUILD_C_API=OFF -DGALAY_ENABLE_CPP23_MODULES=OFF
    -DGALAY_DISABLE_IOURING=ON -DGALAY_BUILD_API=ON
    -DGALAY_BUILD_UTILS=ON -DGALAY_BUILD_KERNEL=ON
    -DGALAY_BUILD_HTTP=ON -DGALAY_BUILD_SERDE=ON)
foreach(module WS REDIS ETCD MYSQL POSTGRES MONGO RPC MCP TRACING)
    list(APPEND flags -DGALAY_BUILD_${module}=OFF)
endforeach()

function(run_step log)
    string(JOIN " " command ${ARGN})
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result
        OUTPUT_VARIABLE output ERROR_VARIABLE error)
    file(WRITE "${log}" "${command}\n${output}\n${error}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${command} failed (${result}): ${output}\n${error}")
    endif()
endfunction()

foreach(ssl OFF ON)
    foreach(http2 OFF ON)
        set(name "ssl-${ssl}-http2-${http2}")
        set(build "${work}/${name}")
        run_step("${work}/${name}-configure.log"
            "${CMAKE_COMMAND}" -S "${GALAY_SOURCE_DIR}" -B "${build}"
            ${flags} -DGALAY_BUILD_SSL=${ssl} -DGALAY_BUILD_HTTP2=${http2})
        if(NOT ssl)
            file(READ "${build}/CMakeCache.txt" cache)
            if(cache MATCHES "OPENSSL_[A-Z_]+:")
                message(FATAL_ERROR "${name} unexpectedly requires an OpenSSL library")
            endif()
        endif()
        run_step("${work}/${name}-build.log"
            "${CMAKE_COMMAND}" --build "${build}" --target test/cpp/api/all --parallel 1)
        run_step("${work}/${name}-ctest.log"
            "${CMAKE_CTEST_COMMAND}" --test-dir "${build}" -R "^api\\." --output-on-failure)
        message(STATUS "${name}: -Werror build and all API tests, including relocated installation, passed")
    endforeach()
endforeach()
