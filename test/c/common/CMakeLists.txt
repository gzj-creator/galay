add_executable(test_c_common_header_smoke header_smoke.c)
target_include_directories(test_c_common_header_smoke
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(test_c_common_header_smoke PRIVATE galay::c-common)
add_test(NAME c.common.header_smoke COMMAND test_c_common_header_smoke)

add_executable(test_c_common_abi_version_smoke abi_version_smoke.c)
target_include_directories(test_c_common_abi_version_smoke
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(test_c_common_abi_version_smoke PRIVATE galay::c-common)
set_target_properties(test_c_common_abi_version_smoke PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
add_test(NAME c.common.abi_version_smoke COMMAND test_c_common_abi_version_smoke)
