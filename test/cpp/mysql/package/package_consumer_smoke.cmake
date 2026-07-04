cmake_minimum_required(VERSION 3.20)

foreach(required_var
        IN ITEMS
        GALAY_MYSQL_SOURCE_DIR
        GALAY_MYSQL_BINARY_DIR
        GALAY_MYSQL_CMAKE_GENERATOR
        GALAY_MYSQL_CXX_COMPILER)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "PackageConsumerSmoke requires `${required_var}`.")
    endif()
endforeach()

if(NOT DEFINED GALAY_MYSQL_CXX_STANDARD OR "${GALAY_MYSQL_CXX_STANDARD}" STREQUAL "")
    set(GALAY_MYSQL_CXX_STANDARD 23)
endif()

set(smoke_root "${GALAY_MYSQL_BINARY_DIR}/test/package-smoke")
set(prefix_dir "${smoke_root}/prefix")
set(consumer_source_dir "${smoke_root}/consumer")
set(consumer_build_dir "${smoke_root}/consumer-build")

file(REMOVE_RECURSE "${smoke_root}")
file(MAKE_DIRECTORY "${consumer_source_dir}")

file(WRITE "${consumer_source_dir}/main.cc"
    "#include <galay/cpp/galay-mysql/sync/mysql_client.h>\n"
    "int main() { return 0; }\n")

configure_file(
    "${GALAY_MYSQL_SOURCE_DIR}/test/cpp/mysql/package/CMakeLists.txt.in"
    "${consumer_source_dir}/CMakeLists.txt"
    @ONLY
)

set(install_command
    "${CMAKE_COMMAND}" --install "${GALAY_MYSQL_BINARY_DIR}" --prefix "${prefix_dir}")
if(DEFINED GALAY_MYSQL_INSTALL_CONFIG AND NOT "${GALAY_MYSQL_INSTALL_CONFIG}" STREQUAL "")
    list(APPEND install_command --config "${GALAY_MYSQL_INSTALL_CONFIG}")
endif()

execute_process(
    COMMAND ${install_command}
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr
)

if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to install galay for package smoke test.\n"
        "stdout:\n${install_stdout}\n"
        "stderr:\n${install_stderr}")
endif()

foreach(required_package_file
        IN ITEMS
        "lib/cmake/galay/galayConfig.cmake"
        "lib/cmake/galay/galayConfigVersion.cmake"
        "lib/cmake/galay/galayTargets.cmake")
    if(NOT EXISTS "${prefix_dir}/${required_package_file}")
        message(FATAL_ERROR "Missing installed package file: ${required_package_file}")
    endif()
endforeach()

file(GLOB installed_module_package_dirs
    "${prefix_dir}/lib/cmake/galay-*")
if(installed_module_package_dirs)
    message(FATAL_ERROR
        "Installed CMake package layout must expose only lib/cmake/galay, "
        "not per-module package directories: ${installed_module_package_dirs}")
endif()

set(package_prefix_path "${prefix_dir}")
if(DEFINED GALAY_MYSQL_PACKAGE_CMAKE_PREFIX_PATH
   AND NOT "${GALAY_MYSQL_PACKAGE_CMAKE_PREFIX_PATH}" STREQUAL "")
    set(package_prefix_path
        "${prefix_dir};${GALAY_MYSQL_PACKAGE_CMAKE_PREFIX_PATH}")
endif()

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${consumer_source_dir}"
    -B "${consumer_build_dir}"
    -G "${GALAY_MYSQL_CMAKE_GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${GALAY_MYSQL_CXX_COMPILER}"
    "-DCMAKE_CXX_STANDARD=${GALAY_MYSQL_CXX_STANDARD}"
    "-DCMAKE_PREFIX_PATH=${package_prefix_path}")

if(DEFINED GALAY_MYSQL_BUILD_TYPE AND NOT "${GALAY_MYSQL_BUILD_TYPE}" STREQUAL "")
    list(APPEND configure_command "-DCMAKE_BUILD_TYPE=${GALAY_MYSQL_BUILD_TYPE}")
endif()

execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
)

if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to configure external consumer via find_package(galay).\n"
        "stdout:\n${configure_stdout}\n"
        "stderr:\n${configure_stderr}")
endif()

set(build_command "${CMAKE_COMMAND}" --build "${consumer_build_dir}")
if(DEFINED GALAY_MYSQL_INSTALL_CONFIG AND NOT "${GALAY_MYSQL_INSTALL_CONFIG}" STREQUAL "")
    list(APPEND build_command --config "${GALAY_MYSQL_INSTALL_CONFIG}")
endif()

execute_process(
    COMMAND ${build_command}
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
)

if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to build external consumer linked against installed galay.\n"
        "stdout:\n${build_stdout}\n"
        "stderr:\n${build_stderr}")
endif()
