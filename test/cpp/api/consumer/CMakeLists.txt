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

add_executable(users e1_users.cc)
target_link_libraries(users PRIVATE galay::api)
add_test(NAME installed.export COMMAND users --export "${CMAKE_CURRENT_BINARY_DIR}/openapi.json")
