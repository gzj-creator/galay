cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(galay_c_ssl_tests LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
    include(CTest)
    include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/dependencies.cmake)
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/include/galay)
    file(CREATE_LINK
        ${CMAKE_CURRENT_LIST_DIR}/../../../src/cpp
        ${CMAKE_BINARY_DIR}/include/galay/cpp
        SYMBOLIC
        COPY_ON_ERROR
    )
    file(CREATE_LINK
        ${CMAKE_CURRENT_LIST_DIR}/../../../src/c
        ${CMAKE_BINARY_DIR}/include/galay/c
        SYMBOLIC
        COPY_ON_ERROR
    )
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../src/cpp/galay-utils src_cpp_galay_utils)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../src/cpp/galay-kernel src_cpp_galay_kernel)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../src/cpp/galay-ssl src_cpp_galay_ssl)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../src/c/galay-common-c src_c_galay_c)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../src/c/galay-ssl-c src_c_galay_ssl)
endif()

function(galay_add_c_ssl_test source_file)
    get_filename_component(scenario_name "${source_file}" NAME_WE)
    set(target_name "test_c_ssl_${scenario_name}")
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::c-ssl)
    if(scenario_name STREQUAL "context_boundary")
        target_compile_definitions(${target_name}
            PRIVATE
                GALAY_SSL_TEST_CERT_DIR="${CMAKE_CURRENT_LIST_DIR}/../../cpp/ssl/certs"
        )
    endif()
    add_test(NAME "c.ssl.${scenario_name}" COMMAND ${target_name})
endfunction()

file(GLOB GALAY_C_SSL_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
foreach(test_source IN LISTS GALAY_C_SSL_TEST_SOURCES)
    galay_add_c_ssl_test(${test_source})
endforeach()
