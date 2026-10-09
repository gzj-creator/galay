set(work "${GALAY_BINARY_DIR}/api-install")
set(prefix "${work}/prefix")
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${GALAY_BINARY_DIR}" --prefix "${prefix}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
file(MAKE_DIRECTORY "${work}")
file(WRITE "${work}/install.log" "${output}\n${error}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "API install failed: ${output}\n${error}")
endif()
# The built-in resources must not create a separate installed resource directory.
if(EXISTS "${prefix}/share/galay/swagger-ui")
    message(FATAL_ERROR "Built-in Swagger UI unexpectedly installed a resource directory")
endif()
# Relocate the package before find_package, building and running the consumer.
set(relocated "${work}/relocated-prefix")
file(REMOVE_RECURSE "${relocated}")
file(RENAME "${prefix}" "${relocated}" RESULT result)
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Cannot relocate installed package: ${result}")
endif()
set(prefix "${relocated}")
foreach(removed_header IN ITEMS api_server.h docs.h api_contract.h api_error.h
        binding_contract.h operation.h schema_model.h http2_adapter.h)
    if(EXISTS "${prefix}/include/galay/cpp/galay-api/${removed_header}")
        message(FATAL_ERROR "Removed API surface is still installed: ${removed_header}")
    endif()
endforeach()
file(COPY "${CMAKE_CURRENT_LIST_DIR}/consumer" DESTINATION "${work}")
file(COPY "${GALAY_SOURCE_DIR}/examples/cpp/api/e1_users.cc"
    DESTINATION "${work}/consumer")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/t10_startup.cc"
    "${CMAKE_CURRENT_LIST_DIR}/t11_transport_server.cc"
    "${CMAKE_CURRENT_LIST_DIR}/transport_acceptance.cjs"
    "${CMAKE_CURRENT_LIST_DIR}/startup_acceptance.cmake"
    DESTINATION "${work}/consumer")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${work}/consumer" -B "${work}/build"
    -G Ninja -DCMAKE_CXX_COMPILER=${GALAY_CXX_COMPILER} -DCMAKE_PREFIX_PATH=${prefix}
    -DCMAKE_CXX_FLAGS=-Werror
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
file(WRITE "${work}/configure.log" "${output}\n${error}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installed API consumer configuration failed: ${output}\n${error}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${work}/build" --parallel 1
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
file(WRITE "${work}/build.log" "${output}\n${error}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installed API consumer build failed: ${output}\n${error}")
endif()
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${work}/build" --output-on-failure
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
file(WRITE "${work}/ctest.log" "${output}\n${error}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installed API consumer test failed: ${output}\n${error}")
endif()
message(STATUS "Relocated installed API consumer built and ran without share using only find_package(galay) / galay::api")
