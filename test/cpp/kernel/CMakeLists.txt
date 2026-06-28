# 测试可执行文件 - 自动扫描目录

# 扫描 test/ 下所有测试源文件，并统一注册到 CTest
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "*.cc")

set(GALAY_KERNEL_SCENARIO_t2_tcp tcp_basic)
set(GALAY_KERNEL_SCENARIO_t3_tcp tcp_server)
set(GALAY_KERNEL_SCENARIO_t4_tcp tcp_client)
set(GALAY_KERNEL_SCENARIO_t5_udp udp_basic)
set(GALAY_KERNEL_SCENARIO_t6_udp udp_server)
set(GALAY_KERNEL_SCENARIO_t7_udp udp_client)
set(GALAY_KERNEL_SCENARIO_t17_iov iov_tcp_readwrite)
set(GALAY_KERNEL_SCENARIO_t76_iov iov_awaitable_surface)
set(GALAY_KERNEL_SCENARIO_t23_await await_benchmark)
set(GALAY_KERNEL_SCENARIO_t25_await await_sequence)
set(GALAY_KERNEL_SCENARIO_t61_await await_task)
set(GALAY_KERNEL_SCENARIO_t32_batchrecv batchrecv_progress)
set(GALAY_KERNEL_SCENARIO_t33_batchrecv batchrecv_stress)
set(GALAY_KERNEL_SCENARIO_t54_then then_runtime)
set(GALAY_KERNEL_SCENARIO_t62_then then_surface)
set(GALAY_KERNEL_SCENARIO_t89_awready awready_surface)
set(GALAY_KERNEL_SCENARIO_t90_awready awready_runtime)

# 为每个测试文件创建可执行文件
foreach(TEST_SOURCE ${TEST_SOURCES})
    # 获取文件名（不含路径和扩展名）
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
    if(TEST_NAME STREQUAL "t137_c_coro_resume_token" AND NOT GALAY_C_CORO_CONTEXT_SUPPORTED)
        message(STATUS "Skipping ${TEST_NAME}: ${GALAY_C_CORO_CONTEXT_UNSUPPORTED_REASON}")
        continue()
    endif()

    # 创建可执行文件
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME} PRIVATE galay::kernel)
    target_include_directories(${TEST_NAME}
        PRIVATE
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_BINARY_DIR}/include
    )
    target_compile_definitions(${TEST_NAME}
        PRIVATE
            GALAY_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
            GALAY_SOURCE_ROOT="${CMAKE_SOURCE_DIR}/src/cpp"
    )
    if(TEST_NAME STREQUAL "t122_taskresult_storage")
        target_compile_options(${TEST_NAME} PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-Werror=free-nonheap-object>)
    endif()
    if(TEST_NAME STREQUAL "t137_c_coro_resume_token")
        target_link_libraries(${TEST_NAME} PRIVATE galay::c-kernel)
    endif()

    string(REGEX REPLACE "^t[0-9]+_" "" SCENARIO_NAME "${TEST_NAME}")
    set(SCENARIO_OVERRIDE "GALAY_KERNEL_SCENARIO_${TEST_NAME}")
    if(DEFINED "${SCENARIO_OVERRIDE}")
        set(SCENARIO_NAME "${${SCENARIO_OVERRIDE}}")
    endif()
    set(CTEST_NAME "kernel.${SCENARIO_NAME}")
    add_test(NAME ${CTEST_NAME} COMMAND $<TARGET_FILE:${TEST_NAME}>)
    set_tests_properties(${CTEST_NAME} PROPERTIES WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endforeach()
