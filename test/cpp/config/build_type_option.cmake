cmake_minimum_required(VERSION 3.20)

foreach(required_var
        IN ITEMS
        GALAY_SOURCE_DIR
        GALAY_BINARY_DIR
        GALAY_CMAKE_GENERATOR
        GALAY_CXX_COMPILER)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "build_type_option requires `${required_var}`.")
    endif()
endforeach()

if(GALAY_CMAKE_GENERATOR MATCHES "Visual Studio|Xcode|Multi-Config")
    return()
endif()

set(smoke_root "${GALAY_BINARY_DIR}/test/build-type-option")
set(release_build_dir "${smoke_root}/release-build")
set(debug_build_dir "${smoke_root}/debug-build")
set(relwithdebinfo_build_dir "${smoke_root}/relwithdebinfo-build")
file(REMOVE_RECURSE "${smoke_root}")

set(common_configure_args
    -S "${GALAY_SOURCE_DIR}"
    -G "${GALAY_CMAKE_GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${GALAY_CXX_COMPILER}"
    -DBUILD_TESTING=OFF
    -DGALAY_BUILD_KERNEL=OFF
    -DGALAY_BUILD_SSL=OFF
    -DGALAY_BUILD_HTTP=OFF
    -DGALAY_BUILD_WS=OFF
    -DGALAY_BUILD_HTTP2=OFF
    -DGALAY_BUILD_REDIS=OFF
    -DGALAY_BUILD_ETCD=OFF
    -DGALAY_BUILD_MONGO=OFF
    -DGALAY_BUILD_MYSQL=OFF
    -DGALAY_BUILD_RPC=OFF
    -DGALAY_BUILD_MCP=OFF
    -DGALAY_BUILD_TRACING=OFF)

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${common_configure_args}
        -B "${release_build_dir}"
    RESULT_VARIABLE release_configure_result
    OUTPUT_VARIABLE release_configure_stdout
    ERROR_VARIABLE release_configure_stderr)
if(NOT release_configure_result EQUAL 0)
    message(FATAL_ERROR
        "Default Release configure failed.\n"
        "stdout:\n${release_configure_stdout}\n"
        "stderr:\n${release_configure_stderr}")
endif()

file(STRINGS "${release_build_dir}/CMakeCache.txt" release_build_type
    REGEX "^CMAKE_BUILD_TYPE:STRING=")
if(NOT release_build_type STREQUAL "CMAKE_BUILD_TYPE:STRING=Release")
    message(FATAL_ERROR
        "Default build must use Release, got `${release_build_type}`.")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${common_configure_args}
        -B "${debug_build_dir}"
        -DGALAY_BUILD_DEBUG=ON
    RESULT_VARIABLE debug_configure_result
    OUTPUT_VARIABLE debug_configure_stdout
    ERROR_VARIABLE debug_configure_stderr)
if(NOT debug_configure_result EQUAL 0)
    message(FATAL_ERROR
        "Debug configure failed.\n"
        "stdout:\n${debug_configure_stdout}\n"
        "stderr:\n${debug_configure_stderr}")
endif()

file(STRINGS "${debug_build_dir}/CMakeCache.txt" debug_build_type
    REGEX "^CMAKE_BUILD_TYPE:STRING=")
if(NOT debug_build_type STREQUAL "CMAKE_BUILD_TYPE:STRING=Debug")
    message(FATAL_ERROR
        "GALAY_BUILD_DEBUG=ON must use Debug, got `${debug_build_type}`.")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${common_configure_args}
        -B "${relwithdebinfo_build_dir}"
        -DGALAY_BUILD_DEBUG=OFF
        -DCMAKE_BUILD_TYPE=RelWithDebInfo
    RESULT_VARIABLE relwithdebinfo_configure_result
    OUTPUT_VARIABLE relwithdebinfo_configure_stdout
    ERROR_VARIABLE relwithdebinfo_configure_stderr)
if(NOT relwithdebinfo_configure_result EQUAL 0)
    message(FATAL_ERROR
        "RelWithDebInfo configure failed.\n"
        "stdout:\n${relwithdebinfo_configure_stdout}\n"
        "stderr:\n${relwithdebinfo_configure_stderr}")
endif()

file(STRINGS "${relwithdebinfo_build_dir}/CMakeCache.txt" relwithdebinfo_build_type
    REGEX "^CMAKE_BUILD_TYPE:STRING=")
if(NOT relwithdebinfo_build_type STREQUAL "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo")
    message(FATAL_ERROR
        "Explicit RelWithDebInfo must be preserved, got `${relwithdebinfo_build_type}`.")
endif()
