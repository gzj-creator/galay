# Galay

[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)
[![C++ Standard](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![CMake](https://img.shields.io/badge/CMake-3.21%2B-blue.svg)](https://cmake.org/)

Galay 是一个高性能、跨平台的 C++23 网络库，提供支持协程的异步 I/O 操作。它采用现代 C++ 设计，支持 TCP、UDP 和 SSL/TLS 协议，并集成了先进的 I/O 机制，包括epoll 和 kqueue。

## 特性

### 🚀 **高性能**
- **现代 I/O 机制**：自动检测并使用 epoll、kqueue
- **协程支持**：内置 C++23 协程支持，用于异步编程
- **零拷贝操作**：高效的内存管理，最小化数据拷贝
- **多线程运行时**：基于线程池的执行，实现最优性能

### 🌐 **网络协议**
- **TCP 服务器/客户端**：功能完整的 TCP 网络通信，支持异步操作
- **UDP 服务器/客户端**：高性能 UDP 通信
- **SSL/TLS 支持**：通过 OpenSSL 集成实现安全连接
- **跨平台**：支持 Linux、macOS

### 🛠 **丰富的工具集**
- **加密算法**：MD5、SHA256、SHA512、Base64 编码/解码
- **日志系统**：基于 spdlog 的集成日志，支持可配置级别
- **命令行解析器**：现代参数解析，支持类型安全
- **错误处理**：全面的错误管理系统
- **信号处理**：优雅的信号处理，支持堆栈跟踪

### 📦 **现代 C++ 设计**
- **C++23 标准**：使用最新的 C++ 特性，包括概念和协程
- **头文件组件**：许多工具以头文件库的形式提供
- **模板元编程**：高级类型安全编程模式
- **RAII**：自动清理的资源管理

## 快速开始

### 前置要求

- **C++23 兼容编译器**：GCC 10+、Clang 12+ 或 MSVC 2019+
- **CMake 3.21+**
- **OpenSSL**：用于 SSL/TLS 支持
- **spdlog**：用于日志功能
- **concurrentqueue**：用于无锁数据结构
- **libcuckoo**：用于高性能哈希表
- **libaio**：用于文件io(linux 依赖)

### 安装

#### Linux/macOS

```bash
# 克隆仓库
git clone https://github.com/your-username/galay.git
cd galay

# 创建构建目录
mkdir build && cd build

# 使用 CMake 配置
cmake .. -DCMAKE_BUILD_TYPE=Release

# 构建库
make -j$(nproc)

# 安装（可选）
sudo make install
```

#### Windows

```cmd
# 克隆仓库
git clone https://github.com/your-username/galay.git
cd galay

# 创建构建目录
mkdir build && cd build

# 使用 CMake 配置
cmake .. -G "Visual Studio 16 2019" -A x64

# 构建库
cmake --build . --config Release
```

### 基本使用

#### TCP 服务器示例

```cpp
#include "galay/kernel/server/TcpServer.h"

using namespace galay;

int main() {
    TcpServerBuilder builder;
    builder.addListen({"0.0.0.0", 8080});
    
    TcpServer server = builder
        .startCoChecker(true, std::chrono::milliseconds(1000))
        .build();
    
    server.run([](AsyncTcpSocket& socket) -> Coroutine<nil> {
        while(true) {
            auto rwrapper = co_await socket.recv(1024);
            if(!rwrapper.success()) {
                if(rwrapper.getError()->code() == error::ErrorCode::DisConnectError) {
                    co_await socket.close();
                    co_return nil();
                }
                co_return nil();
            }
            
            Bytes bytes = rwrapper.moveValue();
            std::string msg = bytes.toString();
            std::cout << "收到消息: " << msg << std::endl;
            
            // 回显消息
            auto wwrapper = co_await socket.send(std::move(bytes));
            if(!wwrapper.success()) {
                std::cout << "发送错误: " << wwrapper.getError()->message() << std::endl;
            }
        }
    });
    
    server.wait();
    return 0;
}
```

#### SSL/TLS 服务器示例

```cpp
#include "galay/kernel/server/TcpSslServer.h"

using namespace galay;

int main() {
    TcpSslServerBuilder builder;
    builder.sslConf("server.crt", "server.key");
    builder.addListen({"0.0.0.0", 8443});
    
    TcpSslServer server = builder
        .startCoChecker(true, std::chrono::milliseconds(1000))
        .build();
    
    server.run([](AsyncSslSocket& socket) -> Coroutine<nil> {
        while(true) {
            auto rwrapper = co_await socket.sslRecv(1024);
            if(!rwrapper.success()) {
                if(rwrapper.getError()->code() == error::ErrorCode::DisConnectError) {
                    co_await socket.sslClose();
                    co_return nil();
                }
                co_return nil();
            }
            
            Bytes bytes = rwrapper.moveValue();
            auto wwrapper = co_await socket.sslSend(std::move(bytes));
        }
    });
    
    server.wait();
    return 0;
}
```

#### UDP 服务器示例

```cpp
#include "galay/kernel/server/UdpServer.h"

using namespace galay;

int main() {
    UdpServerBuilder builder;
    builder.addListen({"0.0.0.0", 8080});
    
    UdpServer server = builder
        .startCoChecker(true, std::chrono::milliseconds(1000))
        .build();
    
    server.run([](AsyncUdpSocket& socket) -> Coroutine<nil> {
        while(true) {
            Host remote;
            auto rwrapper = co_await socket.recvfrom(remote, 1024);
            if(!rwrapper.success()) {
                co_return nil();
            }
            
            Bytes bytes = rwrapper.moveValue();
            std::cout << "收到来自 " << remote.ip << ":" << remote.port << " 的消息" << std::endl;
            
            // 回显给发送者
            auto wwrapper = co_await socket.sendto(remote, std::move(bytes));
        }
    });
    
    server.wait();
    return 0;
}
```

## API文档

所有公共函数和类都使用Doxygen风格的中文详细注释。主要API包括：

### 算法模块
- `Base64Util`：Base64编解码，支持PEM、MIME和URL安全变体
- `Md5Util::encode()`：MD5哈希生成
- `Sha256Util::encode()`：SHA-256哈希生成
- `Sha512Util::encode()`：SHA-512哈希生成
- `Salt::create()`：安全随机盐值生成
- `murmurHash3_*()`：快速非加密哈希

### 网络模块
- `TcpServer/TcpClient`：基于协程的异步TCP网络
- `UdpServer/UdpClient`：异步UDP网络
- `TcpSslServer/TcpSslClient`：安全SSL/TLS连接
- `Bytes`：高效零拷贝字节容器

### 通用模块
- `Buffer`：动态内存缓冲区，支持容量管理
- `Logger`：灵活的日志系统，支持多种输出格式
- `CommonError`：全面的错误处理

### 工具模块
- `ScrambleThreadPool`：高性能线程池
- `RateLimiter`：令牌桶限流器
- `ParserManager`：配置文件解析（JSON、自定义格式）

详细的API参考请查看源代码内联文档。

## 架构

### 核心模块

#### 🧮 **算法模块**
- **加密功能**：MD5、SHA256、SHA512 哈希
- **编码/解码**：Base64 多种变体（PEM、MIME、URL 安全）
- **哈希函数**：MurmurHash3 快速哈希
- **盐值生成**：安全的随机盐值生成

#### 🔧 **通用模块**
- **基础类型**：跨平台句柄定义和事件类型
- **错误处理**：全面的错误管理系统
- **日志**：基于 spdlog 的集成日志
- **反射**：运行时类型信息和序列化
- **策略模式**：可插拔的算法实现

#### ⚡ **内核模块**
- **异步 I/O**：文件、网络和定时器事件处理
- **协程**：C++23 协程支持异步操作
- **事件循环**：高性能事件驱动架构
- **运行时**：线程池和调度器管理
- **服务器**：TCP、UDP 和 SSL 服务器实现
- **客户端**：TCP、UDP 和 SSL 客户端实现

#### 🛠 **工具模块**
- **应用框架**：命令行参数解析
- **堆栈跟踪**：用于调试的堆栈跟踪生成
- **熔断器**：容错模式
- **分布式系统**：分布式计算工具
- **数据结构**：池、树和专用容器
- **限流**：流量控制和节流
- **字符串工具**：高级字符串操作
- **系统信息**：平台特定的系统调用

## 构建配置

### CMake 选项

| 选项 | 默认值 | 描述 |
|------|--------|------|
| `ENABLE_DEBUG` | ON | 启用调试符号和断言 |
| `ENABLE_INSTALL_SYSTEM` | ON | 安装到系统目录 |
| `BUILD_STATIC` | OFF | 构建静态库而非共享库 |
| `ENABLE_SYSTEM_LOG` | ON | 启用系统日志 |
| `ENABLE_LOG_TRACE` | ON | 启用跟踪级别日志 |
| `ENABLE_DEFAULT_USE_EPOLL` | ON | 在 Linux 上强制使用 epoll |
| `ENABLE_GTEST` | AUTO | 启用 Google Test 集成 |
| `ENABLE_NLOHMANN_JSON` | AUTO | 启用 JSON 支持 |

### 平台特定的 I/O

库会自动检测并使用最佳的可用 I/O 机制：

- **Linux**：epoll
- **macOS/FreeBSD**：kqueue

## 测试

项目包含所有主要组件的综合测试套件：

### 单元测试

```bash
# 构建测试
cd build
make

# 运行单元测试
cd test
./test_algorithm_unit    # 算法模块单元测试（Base64、MD5、SHA256等）
./test_buffer_unit       # Buffer和Bytes单元测试
```

### 集成测试

```bash
# 运行集成测试
./test_tcp_server    # TCP 服务器功能
./test_ssl_server    # SSL/TLS 服务器功能
./test_udp_server    # UDP 服务器功能
./test_tcp_client    # TCP 客户端功能
./test_ssl_client    # SSL/TLS 客户端功能
./test_udp_client    # UDP 客户端功能
./test_file          # 文件 I/O 操作
./test_time          # 定时器功能
```

### 压力测试

```bash
# 运行压力测试
./test_stress_tcp           # TCP服务器压力测试（1000并发客户端）
./test_stress_threadpool    # 线程池压力测试（10万任务）
```

### 测试覆盖

- **算法模块**：Base64编解码、MD5、SHA256、SHA512、盐值生成、MurmurHash3
- **缓冲区模块**：Buffer创建、容量管理、数据转换
- **字节模块**：Bytes构造、数据访问、转换、比较
- **网络模块**：TCP/UDP/SSL服务器和客户端功能
- **性能测试**：高并发和吞吐量测试

## 性能

Galay 专为高性能网络应用而设计：

- **零拷贝 I/O**：最小化内存拷贝操作
- **无锁数据结构**：使用 concurrentqueue 和 libcuckoo
- **现代 I/O**：在支持的 Linux 内核上利用 io_uring
- **协程效率**：无栈协程，开销最小
- **线程池**：通过工作窃取实现最优 CPU 利用率

### 开发环境设置

```bash
# 安装依赖（Ubuntu/Debian）
sudo apt-get install libssl-dev libspdlog-dev libgtest-dev

# 安装 concurrentqueue
git clone https://github.com/cameron314/concurrentqueue.git
sudo cp -r concurrentqueue/concurrentqueue /usr/local/include/

# 安装 libcuckoo
git clone https://github.com/efficient/libcuckoo.git
sudo cp -r libcuckoo/libcuckoo /usr/local/include/

# 使用调试符号构建
mkdir build && cd build
cmake .. -DENABLE_DEBUG=ON
make -j$(nproc)
```

### 构建测试

构建和运行测试：

```bash
cd build
make

# 运行所有单元测试
cd test
./test_algorithm_unit
./test_buffer_unit

# 运行压力测试
./test_stress_tcp
./test_stress_threadpool
```

### 代码文档

所有公共API都包含详细文档：
- **@brief**：简短功能描述
- **@details**：详细说明
- **@param**：参数描述
- **@return**：返回值描述
- **@throw**：异常信息

文档使用简体中文编写，便于中国开发者阅读。

## 许可证

本项目采用 Apache License 2.0 许可证 - 详情请参阅 [LICENSE](LICENSE) 文件。


## 路线图

- [ ] IOCP支持
- [ ] io_uring支持

---

**Galay** - 支持协程的高性能 C++23 网络库。
