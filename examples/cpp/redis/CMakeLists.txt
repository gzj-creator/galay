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

file(GLOB GALAY_REDIS_INCLUDE_EXAMPLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/*.cc")
foreach(example_source IN LISTS GALAY_REDIS_INCLUDE_EXAMPLE_SOURCES)
    get_filename_component(example_name "${example_source}" NAME_WE)
    string(REGEX REPLACE "^e[0-9]+_" "" scenario_name "${example_name}")
    galay_add_redis_example(example_redis_include_${scenario_name} ${example_source})
endforeach()

file(GLOB GALAY_REDIS_IMPORT_EXAMPLE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/import/*.cc")
foreach(example_source IN LISTS GALAY_REDIS_IMPORT_EXAMPLE_SOURCES)
    get_filename_component(example_name "${example_source}" NAME_WE)
    string(REGEX REPLACE "^e[0-9]+_" "" scenario_name "${example_name}")
    galay_add_redis_import_example(example_redis_import_${scenario_name} ${example_source})
endforeach()

file(GLOB GALAY_REDIS_MANUAL_EXAMPLE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/include/manual/*.cc"
)
foreach(example_source IN LISTS GALAY_REDIS_MANUAL_EXAMPLE_SOURCES)
    get_filename_component(example_name "${example_source}" NAME_WE)
    galay_add_redis_example(example_redis_manual_${example_name} ${example_source})
endforeach()
