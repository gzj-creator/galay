function(galay_add_redis_example target_name source_file)
    add_executable(${target_name} ${source_file})
    set_property(TARGET ${target_name} PROPERTY NO_SYSTEM_FROM_IMPORTED ON)
    target_link_libraries(${target_name} PRIVATE galay::redis)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        INSTALL_RPATH $ORIGIN/../lib
    )
endfunction()

function(galay_add_redis_import_example target_name source_file)
    if(NOT TARGET galay-redis-modules)
        return()
    endif()
    galay_add_redis_example(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay-redis-modules)
    set_target_properties(${target_name} PROPERTIES CXX_SCAN_FOR_MODULES ON)
endfunction()

galay_add_redis_example(example_redis_include_basic include/e1_basic.cc)
galay_add_redis_example(example_redis_include_pipeline include/e2_pipeline.cc)
galay_add_redis_example(example_redis_include_pubsub include/e3_pubsub.cc)

galay_add_redis_import_example(example_redis_import_basic import/e1_basic.cc)
galay_add_redis_import_example(example_redis_import_pipeline import/e2_pipeline.cc)
galay_add_redis_import_example(example_redis_import_pubsub import/e3_pubsub.cc)
