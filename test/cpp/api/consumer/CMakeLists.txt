cmake_minimum_required(VERSION 3.28)
project(galay_api_consumer LANGUAGES CXX)
find_package(galay CONFIG REQUIRED)
enable_testing()

add_executable(consumer t2_install_consumer.cc)
target_link_libraries(consumer PRIVATE galay::api)
target_compile_options(consumer PRIVATE -UNDEBUG)
add_test(NAME installed.api COMMAND consumer)

add_executable(loopback t1_loopback.cc)
target_link_libraries(loopback PRIVATE galay::api)
add_test(NAME installed.loopback COMMAND loopback)
set_tests_properties(installed.loopback PROPERTIES TIMEOUT 60)

add_executable(no_docs t1_loopback.cc)
target_link_libraries(no_docs PRIVATE galay::api)
target_compile_definitions(no_docs PRIVATE GALAY_CONSUMER_NO_DOCS)
add_test(NAME installed.no_docs COMMAND no_docs)
set_tests_properties(installed.no_docs PROPERTIES TIMEOUT 60)

add_executable(users e1_users.cc)
target_link_libraries(users PRIVATE galay::api)
if(TARGET galay::http2)
    target_link_libraries(users PRIVATE galay::http2)
endif()
add_test(NAME installed.export COMMAND users --export "${CMAKE_CURRENT_BINARY_DIR}/openapi.json")

find_program(node node REQUIRED)
add_executable(transport_server t11_transport_server.cc)
target_link_libraries(transport_server PRIVATE galay::api)
if(TARGET galay::http2)
    target_link_libraries(transport_server PRIVATE galay::http2)
endif()
set(transports http)
if(TARGET galay::ssl)
    string(APPEND transports ",https")
endif()
if(TARGET galay::http2)
    string(APPEND transports ",h2c")
    if(TARGET galay::ssl)
        string(APPEND transports ",h2")
    endif()
endif()
add_test(NAME installed.transports COMMAND ${node}
    ${CMAKE_CURRENT_SOURCE_DIR}/transport_acceptance.cjs $<TARGET_FILE:transport_server>
    ${CONSUMER_REFERENCE_ASSETS} ${CMAKE_CURRENT_BINARY_DIR}/transport-evidence ${transports})
set_tests_properties(installed.transports PROPERTIES TIMEOUT 180)

add_executable(startup t10_startup.cc)
target_link_libraries(startup PRIVATE galay::api)
if(TARGET galay::http2)
    target_link_libraries(startup PRIVATE galay::http2)
endif()
add_test(NAME installed.startup COMMAND ${CMAKE_COMMAND}
    -DTEST_BINARY=$<TARGET_FILE:startup>
    -DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
    -DTEST_WITH_SSL=$<BOOL:$<TARGET_EXISTS:galay::ssl>>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/startup_acceptance.cmake)
set_tests_properties(installed.startup PROPERTIES TIMEOUT 60)
