file(GLOB GALAY_RPC_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
list(REMOVE_ITEM GALAY_RPC_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/t92_module_smoke.cc")
list(REMOVE_ITEM GALAY_RPC_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/t64_etcd_real_chain.cc")
list(REMOVE_ITEM GALAY_RPC_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/t94_rpc_etcd_adapter_surface.cc")
set(GALAY_RPC_TEST_SKIP_RETURN_CODE 125)

function(galay_add_rpc_test source_file)
    get_filename_component(target_name "${source_file}" NAME_WE)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::rpc)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include)
    string(REGEX REPLACE "_" "." scenario_name "${target_name}")
    add_test(NAME "rpc.${scenario_name}" COMMAND $<TARGET_FILE:${target_name}>)
    set(label unit)
    if(target_name MATCHES "_surface$")
        set(label surface)
    endif()
    set_tests_properties("rpc.${scenario_name}" PROPERTIES LABELS "rpc;${label}")
endfunction()

add_library(result_writer INTERFACE)
target_include_directories(result_writer INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

foreach(test_source IN LISTS GALAY_RPC_TEST_SOURCES)
    galay_add_rpc_test(${test_source})
endforeach()

if(TARGET galay::etcd)
    if(NOT TARGET galay::rpc-etcd)
        message(FATAL_ERROR "galay::rpc-etcd target is required when rpc and etcd are both enabled")
    endif()

    galay_add_rpc_test("${CMAKE_CURRENT_SOURCE_DIR}/t64_etcd_real_chain.cc")
    target_link_libraries(t64_etcd_real_chain PRIVATE galay::rpc-etcd)
    set_tests_properties(rpc.t64.etcd.real.chain PROPERTIES
        LABELS "rpc;integration;requires-etcd"
        SKIP_RETURN_CODE ${GALAY_RPC_TEST_SKIP_RETURN_CODE}
    )

    galay_add_rpc_test("${CMAKE_CURRENT_SOURCE_DIR}/t94_rpc_etcd_adapter_surface.cc")
    target_link_libraries(t94_rpc_etcd_adapter_surface PRIVATE galay::rpc-etcd)
    set_tests_properties(rpc.t94.rpc.etcd.adapter.surface PROPERTIES LABELS "rpc;surface")
endif()

if(GALAY_ENABLE_CPP23_MODULES AND TARGET galay-rpc-modules)
    add_executable(t92_module_smoke t92_module_smoke.cc)
    target_link_libraries(t92_module_smoke PRIVATE galay-rpc-modules)
    target_include_directories(t92_module_smoke PRIVATE ${CMAKE_BINARY_DIR}/include)
    set_target_properties(t92_module_smoke PROPERTIES CXX_SCAN_FOR_MODULES ON)
    add_test(NAME rpc.t92.module.smoke COMMAND $<TARGET_FILE:t92_module_smoke>)
    set_tests_properties(rpc.t92.module.smoke PROPERTIES LABELS "rpc;module")
endif()
