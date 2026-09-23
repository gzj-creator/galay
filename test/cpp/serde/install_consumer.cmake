set(work "${GALAY_BINARY_DIR}/test/serde-consumer")
set(prefix "${work}/prefix")
file(MAKE_DIRECTORY "${work}/source")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/consumer/CMakeLists.txt"
    "${CMAKE_CURRENT_LIST_DIR}/t11_struct_formats.cc"
    "${CMAKE_CURRENT_LIST_DIR}/t10_import_smoke.cc"
    "${CMAKE_CURRENT_LIST_DIR}/t12_shared_backend.cc"
    "${CMAKE_CURRENT_LIST_DIR}/struct_formats.hpp"
    DESTINATION "${work}/source")

function(run_checked)
    execute_process(COMMAND ${ARGV}
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Consumer check failed (${result}): ${ARGV}\n${out}\n${err}")
    endif()
endfunction()

run_checked("${CMAKE_COMMAND}" --install "${GALAY_BINARY_DIR}"
    --prefix "${prefix}" --config "${GALAY_CONFIG}")
run_checked("${CMAKE_COMMAND}" -S "${work}/source" -B "${work}/build"
    -G "${GALAY_GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${GALAY_COMPILER}"
    "-DCMAKE_MAKE_PROGRAM=${GALAY_MAKE_PROGRAM}"
    "-DCMAKE_BUILD_TYPE=${GALAY_CONFIG}"
    "-DCMAKE_PREFIX_PATH=${prefix}")
run_checked("${CMAKE_COMMAND}" --build "${work}/build" --config "${GALAY_CONFIG}" --parallel 2)
run_checked("${GALAY_CTEST_COMMAND}" --test-dir "${work}/build"
    -C "${GALAY_CONFIG}" --output-on-failure)
