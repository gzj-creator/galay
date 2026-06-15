function(galay_etcd_configure_example_target target_name)
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
endfunction()

function(galay_add_etcd_example target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::etcd)
    target_include_directories(${target_name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
    set_property(TARGET ${target_name} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    galay_etcd_configure_example_target(${target_name})
endfunction()

function(galay_add_etcd_import_example target_name source_file)
    if(NOT TARGET galay-etcd-modules)
        return()
    endif()
    galay_add_etcd_example(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay-etcd-modules)
    set_target_properties(${target_name} PROPERTIES CXX_SCAN_FOR_MODULES ON)
endfunction()

galay_add_etcd_example(example_etcd_include_sync_basic include/e1_basic.cc)
galay_add_etcd_example(example_etcd_include_async_basic include/e2_basic.cc)

if(GALAY_ETCD_IMPORT_COMPILATION_ENABLED)
    galay_add_etcd_import_example(example_etcd_import_sync_basic import/e1_basic.cc)
    galay_add_etcd_import_example(example_etcd_import_async_basic import/e2_basic.cc)
endif()
