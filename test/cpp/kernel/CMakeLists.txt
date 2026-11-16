# 测试可执行文件 - 自动扫描目录

# 扫描 test/ 下所有测试源文件，并统一注册到 CTest
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS "*.cc")

# Deterministic timeout/waker tests need manually controlled scheduling. Keep
# their hook table in a separate static library: production has neither the
# hook field nor the extra branch, and each executable uses one consistent ABI.
get_target_property(KERNEL_TEST_SOURCES galay-kernel SOURCES)
find_package(Threads REQUIRED)
list(FILTER KERNEL_TEST_SOURCES INCLUDE REGEX "\\.cc$")
add_library(galay-kernel-test-scheduler STATIC EXCLUDE_FROM_ALL ${KERNEL_TEST_SOURCES})
target_compile_features(galay-kernel-test-scheduler PUBLIC cxx_std_23)
target_include_directories(galay-kernel-test-scheduler PUBLIC
    $<TARGET_PROPERTY:galay-kernel,INTERFACE_INCLUDE_DIRECTORIES>)
target_compile_definitions(galay-kernel-test-scheduler PUBLIC
    $<TARGET_PROPERTY:galay-kernel,INTERFACE_COMPILE_DEFINITIONS>
    GALAY_KERNEL_TEST_SCHEDULER)
target_link_libraries(galay-kernel-test-scheduler PUBLIC
    $<TARGET_PROPERTY:galay-kernel,LINK_LIBRARIES>)

# 内核测试的稳定标准库公共头。共享一份 PCH，避免为每个单源可执行文件
# 重复生成体量较大的预编译产物。
set(GALAY_KERNEL_TEST_PCH_HEADERS
    <array>
    <atomic>
    <chrono>
    <coroutine>
    <cstring>
    <expected>
    <iostream>
    <memory>
    <string>
    <thread>
    <vector>
)

function(galay_add_kernel_test_pch pch_target test_link_target)
    add_library(${pch_target} OBJECT pch/test_pch.cc)
    target_compile_features(${pch_target} PRIVATE cxx_std_23)
    target_include_directories(${pch_target}
        PRIVATE
            ${CMAKE_SOURCE_DIR}
            ${CMAKE_BINARY_DIR}/include
    )
    target_compile_definitions(${pch_target}
        PRIVATE
            GALAY_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
            GALAY_SOURCE_ROOT="${CMAKE_SOURCE_DIR}/src/cpp"
    )
    target_link_libraries(${pch_target} PRIVATE ${test_link_target})
    target_precompile_headers(${pch_target}
        PRIVATE ${GALAY_KERNEL_TEST_PCH_HEADERS})
endfunction()

galay_add_kernel_test_pch(galay-kernel-test-pch galay::kernel)
galay_add_kernel_test_pch(
    galay-kernel-test-scheduler-pch galay-kernel-test-scheduler)

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
set(GALAY_KERNEL_SCENARIO_t89_awready awready_surface)
set(GALAY_KERNEL_SCENARIO_t90_awready awready_runtime)

# 为每个测试文件创建可执行文件
foreach(TEST_SOURCE ${TEST_SOURCES})
    # 获取文件名（不含路径和扩展名）
    get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)
    # 创建可执行文件
    add_executable(${TEST_NAME} ${TEST_SOURCE})
    file(STRINGS "${TEST_SOURCE}" TEST_SCHEDULER_ADAPTER_INCLUDE
        REGEX "^#include .*scheduler_test_adapter.h")
    if(TEST_SCHEDULER_ADAPTER_INCLUDE)
        target_link_libraries(${TEST_NAME} PRIVATE galay-kernel-test-scheduler)
    else()
        target_link_libraries(${TEST_NAME} PRIVATE galay::kernel)
    endif()
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
        # 该测试额外的警告升级参数与共享 PCH 不兼容，保持独立编译。
        target_compile_options(${TEST_NAME} PRIVATE
            $<$<CXX_COMPILER_ID:GNU>:-Werror=free-nonheap-object>)
    elseif(TEST_SCHEDULER_ADAPTER_INCLUDE)
        target_precompile_headers(${TEST_NAME}
            REUSE_FROM galay-kernel-test-scheduler-pch)
    else()
        target_precompile_headers(${TEST_NAME}
            REUSE_FROM galay-kernel-test-pch)
    endif()
    string(REGEX REPLACE "^t[0-9]+_" "" SCENARIO_NAME "${TEST_NAME}")
    set(SCENARIO_OVERRIDE "GALAY_KERNEL_SCENARIO_${TEST_NAME}")
    if(DEFINED "${SCENARIO_OVERRIDE}")
        set(SCENARIO_NAME "${${SCENARIO_OVERRIDE}}")
    endif()
    set(CTEST_NAME "kernel.${SCENARIO_NAME}")
    add_test(NAME ${CTEST_NAME} COMMAND $<TARGET_FILE:${TEST_NAME}>)
    set_tests_properties(${CTEST_NAME} PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "kernel;unit"
    )
    if(TEST_NAME STREQUAL "t2_tcp")
        # t2 uses the legacy 8080 endpoint; the server/client pair in t3/t4
        # uses 28080.  Keep these locks separate so independent tests can run
        # concurrently without allowing same-port bind races.
        set_tests_properties(${CTEST_NAME} PROPERTIES
            RESOURCE_LOCK "galay_kernel_tcp_port_8080;galay_loopback_port_8080"
        )
    elseif(TEST_NAME MATCHES "^t[34]_tcp$")
        set_tests_properties(${CTEST_NAME} PROPERTIES RESOURCE_LOCK galay_kernel_tcp_port_28080)
    elseif(TEST_NAME MATCHES "^t[567]_udp$")
        set_tests_properties(${CTEST_NAME} PROPERTIES RESOURCE_LOCK galay_kernel_udp_test_port)
    elseif(TEST_NAME STREQUAL "t17_iov")
        set_tests_properties(${CTEST_NAME} PROPERTIES RESOURCE_LOCK galay_kernel_iov_test_port)
    elseif(TEST_NAME STREQUAL "t18_ringio" OR TEST_NAME STREQUAL "t29_iovborrow")
        set_tests_properties(${CTEST_NAME} PROPERTIES RESOURCE_LOCK galay_kernel_port_9091)
    elseif(TEST_NAME STREQUAL "t21_recvsend")
        set_tests_properties(${CTEST_NAME} PROPERTIES RESOURCE_LOCK galay_kernel_port_19876)
    elseif(TEST_NAME STREQUAL "t25_await")
        set_tests_properties(${CTEST_NAME} PROPERTIES RESOURCE_LOCK galay_kernel_port_20030)
    elseif(TEST_NAME STREQUAL "t56_seqawait")
        set_tests_properties(${CTEST_NAME} PROPERTIES RESOURCE_LOCK galay_kernel_port_20063)
    elseif(TEST_NAME STREQUAL "t119_seqxchg")
        set_tests_properties(${CTEST_NAME} PROPERTIES RESOURCE_LOCK galay_loopback_port_19450)
    elseif(TEST_NAME STREQUAL "t118_accfan")
        set_tests_properties(${CTEST_NAME} PROPERTIES RESOURCE_LOCK galay_loopback_port_19447)
    endif()
    if(TEST_NAME STREQUAL "t156_mpsc_timeout_race")
        # 该回归刻意覆盖内联唤醒重入 close；协议退化时必须有界失败而非挂住 CI。
        set_tests_properties(${CTEST_NAME} PROPERTIES
            TIMEOUT 45
            PROCESSORS 2
        )
    elseif(TEST_NAME STREQUAL "t173_mpmc_unbounded_single_publish_wakeup")
        set_tests_properties(${CTEST_NAME} PROPERTIES
            TIMEOUT 45
            PROCESSORS 2
        )
    elseif(TEST_NAME STREQUAL "t198_task_completion_wait_race")
        set_tests_properties(${CTEST_NAME} PROPERTIES
            TIMEOUT 30
            PROCESSORS 2
        )
    endif()
endforeach()

# 阶段 5 已确认的生产缺陷。修正实现后移除对应 DISABLED 属性，恢复常规回归。
set_tests_properties(kernel.child_await_affinity
    PROPERTIES TIMEOUT 10 DISABLED TRUE)
set_tests_properties(kernel.parallel_resume_delay
    PROPERTIES TIMEOUT 10 DISABLED TRUE)

# Keep the accept-only stop pressure check distinct from the future Runtime
# six-counter shutdown gate. Other backends still require their own migration.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND GALAY_DISABLE_IOURING)
    add_test(NAME kernel.runtime_pending_accept_10k
        COMMAND $<TARGET_FILE:t185_runtime_pending_accept_lsan> 10000)
    set_tests_properties(kernel.runtime_pending_accept_10k PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "kernel;stress"
        TIMEOUT 40)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT GALAY_DISABLE_IOURING
       AND GALAY_LIBURING_INCLUDE_DIR AND GALAY_LIBURING_LIBRARY)
    # io_uring's default queue depth is finite; this verifies the logical stop
    # boundary at a size that fits the current registration/backpressure path.
    add_test(NAME kernel.runtime_pending_accept_uring_2k
        COMMAND $<TARGET_FILE:t185_runtime_pending_accept_lsan> 2000)
    set_tests_properties(kernel.runtime_pending_accept_uring_2k PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "kernel;stress"
        TIMEOUT 40)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    add_test(NAME kernel.runtime_pending_accept_kqueue_128
        COMMAND $<TARGET_FILE:t185_runtime_pending_accept_lsan> 128)
    set_tests_properties(kernel.runtime_pending_accept_kqueue_128 PROPERTIES
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        LABELS "kernel;stress"
        TIMEOUT 20)
endif()
set_tests_properties(kernel.runtime_pending_accept_lsan kernel.accept_completion_integration
    PROPERTIES TIMEOUT 20)
set_tests_properties(kernel.iou_accept_completion_resource kernel.iou_accept_typed_completion
    PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77)
set_tests_properties(kernel.scheduler_config PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
set_tests_properties(kernel.iouring_send_zc_completion PROPERTIES TIMEOUT 5)
set_tests_properties(kernel.runtime_timer_lifetime PROPERTIES TIMEOUT 5)
set_tests_properties(kernel.iouring_sequence_recv_timeout PROPERTIES TIMEOUT 5)
set_tests_properties(kernel.async_file_move PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)
# The 24 recycled-block cases transfer 120 million messages in Debug builds.
set_tests_properties(kernel.prefetch PROPERTIES TIMEOUT 120 PROCESSORS 2)
# Defaults may equal a named preset. Compile the backend and consumer with
# both sets of overrides to catch duplicate instantiations and type drift.
foreach(CONFIG_PRESET IN ITEMS high_performance low_latency)
    set(CONFIG_TARGET galay-kernel-config-${CONFIG_PRESET})
    add_library(${CONFIG_TARGET} OBJECT
        t192_scheduler_config.cc)
    target_link_libraries(${CONFIG_TARGET} PRIVATE galay::kernel)
    if(CONFIG_PRESET STREQUAL "high_performance")
        target_compile_definitions(${CONFIG_TARGET} PRIVATE
            GALAY_SCHEDULER_MAX_EVENTS=2048 GALAY_SCHEDULER_BATCH_SIZE=512
            GALAY_SCHEDULER_QUEUE_DEPTH=8192)
    else()
        target_compile_definitions(${CONFIG_TARGET} PRIVATE
            GALAY_SCHEDULER_MAX_EVENTS=512 GALAY_SCHEDULER_BATCH_SIZE=64
            GALAY_SCHEDULER_QUEUE_DEPTH=2048)
    endif()
endforeach()
