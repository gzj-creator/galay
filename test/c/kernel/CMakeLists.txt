function(galay_add_c_kernel_test target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::c-kernel)
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_BINARY_DIR}/include
            ${CMAKE_SOURCE_DIR}/src/c
    )
    set_target_properties(${target_name} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
endfunction()

file(GLOB GALAY_C_KERNEL_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
set(GALAY_C_KERNEL_SOCKET_TESTS
    t3_config_and_socket_handles
    t5_socket_lifetime_boundary
    t10_udp_socket_callbacks
    t18_tcp_timeout_callbacks
    t19_udp_timeout_callbacks
    t25_coro_tcp
    t32_tcp_reuse_port
)
galay_filter_unsupported_c_coro_sources(GALAY_C_KERNEL_TEST_SOURCES)
foreach(test_source IN LISTS GALAY_C_KERNEL_TEST_SOURCES)
    get_filename_component(source_name "${test_source}" NAME_WE)
    if((source_name STREQUAL "t23_coro_task" OR
        source_name STREQUAL "t24_coro_result_wait" OR
        source_name STREQUAL "t25_coro_tcp") AND NOT GALAY_C_CORO_CONTEXT_SUPPORTED)
        message(STATUS "Skipping ${source_name}: ${GALAY_C_CORO_CONTEXT_UNSUPPORTED_REASON}")
        continue()
    endif()
    string(REGEX REPLACE "^t[0-9]+_" "" scenario_name "${source_name}")
    if(source_name STREQUAL "t22_coro_source_boundaries")
        set(scenario_name "${source_name}")
    endif()
    set(target_name "test_c_kernel_${scenario_name}")
    galay_add_c_kernel_test(${target_name} ${test_source})
    if(source_name STREQUAL "t22_coro_source_boundaries")
        target_compile_definitions(${target_name} PRIVATE GALAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    endif()
    add_test(NAME "c.kernel.${scenario_name}" COMMAND ${target_name})
    set_tests_properties("c.kernel.${scenario_name}" PROPERTIES LABELS "c;kernel;unit")
    if(source_name STREQUAL "t15_async_waiter")
        # This legacy race loop is intentionally long; account for its runtime
        # and two active threads while keeping a finite CTest failure budget.
        set_tests_properties("c.kernel.${scenario_name}" PROPERTIES
            TIMEOUT 45
            PROCESSORS 2
        )
    elseif(source_name STREQUAL "t14_async_mutex")
        set_tests_properties("c.kernel.${scenario_name}" PROPERTIES
            TIMEOUT 20
            PROCESSORS 2
        )
    endif()
    if(source_name IN_LIST GALAY_C_KERNEL_SOCKET_TESTS)
        set_tests_properties("c.kernel.${scenario_name}" PROPERTIES SKIP_RETURN_CODE 125)
    endif()
    if(source_name STREQUAL "t34_coro_sleep_concurrency")
        set_tests_properties("c.kernel.${scenario_name}" PROPERTIES
            SKIP_RETURN_CODE 125 TIMEOUT 15)
    endif()
    if(source_name STREQUAL "t23_coro_task" OR
       source_name STREQUAL "t24_coro_result_wait" OR
       source_name STREQUAL "t25_coro_tcp")
        set_tests_properties("c.kernel.${scenario_name}" PROPERTIES RUN_SERIAL TRUE)
    endif()
endforeach()

# libkqueue exercises the native BSD reactor on Linux without a production
# dependency or changing the platform's normal backend.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(Threads REQUIRED)
    find_path(GALAY_TEST_LIBKQUEUE_INCLUDE_DIR sys/event.h PATH_SUFFIXES kqueue)
    find_library(GALAY_TEST_LIBKQUEUE_LIBRARY NAMES kqueue)
    if(GALAY_TEST_LIBKQUEUE_INCLUDE_DIR AND GALAY_TEST_LIBKQUEUE_LIBRARY)
        enable_language(ASM)
        get_target_property(KQUEUE_TEST_SOURCES galay-c-kernel SOURCES)
        add_library(galay-c-kernel-kqueue-test STATIC ${KQUEUE_TEST_SOURCES})
        target_compile_features(galay-c-kernel-kqueue-test PUBLIC c_std_11)
        target_compile_definitions(galay-c-kernel-kqueue-test PRIVATE
            GALAY_C_TEST_KQUEUE GALAY_DISABLE_IOURING=1)
        target_include_directories(galay-c-kernel-kqueue-test
            PUBLIC ${CMAKE_BINARY_DIR}/include
            PRIVATE ${GALAY_TEST_LIBKQUEUE_INCLUDE_DIR})
        target_link_libraries(galay-c-kernel-kqueue-test
            PUBLIC galay::c-common Threads::Threads ${GALAY_TEST_LIBKQUEUE_LIBRARY})
        foreach(KQUEUE_TEST IN ITEMS t2_runtime_lifecycle t25_coro_tcp t26_coro_sleep
                t18_tcp_timeout_callbacks t19_udp_timeout_callbacks t33_scheduler_reactor
                t34_coro_sleep_concurrency t35_coro_sleep_interruption
                t36_coro_scheduler_shutdown)
            set(KQUEUE_TARGET test_c_kernel_kqueue_${KQUEUE_TEST})
            add_executable(${KQUEUE_TARGET} ${KQUEUE_TEST}.c)
            target_link_libraries(${KQUEUE_TARGET} PRIVATE galay-c-kernel-kqueue-test)
            add_test(NAME c.kernel.kqueue_${KQUEUE_TEST} COMMAND ${KQUEUE_TARGET})
            set_tests_properties(c.kernel.kqueue_${KQUEUE_TEST} PROPERTIES
                LABELS "c;kernel;unit;libkqueue" TIMEOUT 15)
        endforeach()
    endif()
endif()
