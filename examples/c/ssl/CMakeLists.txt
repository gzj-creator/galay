cmake_minimum_required(VERSION 3.20)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(galay_c_ssl_examples LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 23)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
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
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../src/c/galay-c src_c_galay_c)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../../src/c/galay-ssl src_c_galay_ssl)
endif()

add_executable(example_c_ssl_context e1_context.c)
target_link_libraries(example_c_ssl_context PRIVATE galay::c-ssl)
