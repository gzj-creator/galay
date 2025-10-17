# Benchmark CMakeLists.txt
# TCP压力测试工具构建配置

cmake_minimum_required(VERSION 3.21)

# 压力测试服务器
add_executable(stress_tcp_server stress_tcp_server.cc)
target_link_libraries(stress_tcp_server galay)
target_include_directories(stress_tcp_server PRIVATE ${CMAKE_SOURCE_DIR})

# 压力测试客户端
add_executable(stress_tcp_client stress_tcp_client.cc)
target_link_libraries(stress_tcp_client galay)
target_include_directories(stress_tcp_client PRIVATE ${CMAKE_SOURCE_DIR})

# 设置C++标准和RPATH
set_target_properties(stress_tcp_server stress_tcp_client PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
    BUILD_RPATH "${CMAKE_BINARY_DIR}"
    INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/lib"
    INSTALL_RPATH_USE_LINK_PATH TRUE
)

# 安装目标（可选）
install(TARGETS stress_tcp_server stress_tcp_client
    RUNTIME DESTINATION bin/benchmark
)

