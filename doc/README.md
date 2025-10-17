# Galay 文档中心

欢迎查阅 Galay 高性能 C++ 网络框架的完整文档。

## 📚 文档导航

### 🏗️ [架构设计文档](ARCHITECTURE.md)

深入了解 Galay 的整体架构和设计理念：

- **设计理念**：协程优先、零拷贝、事件驱动、模块化分层
- **系统架构**：完整的架构图和组件关系
- **核心组件**：Runtime、EventScheduler、CoroutineScheduler 等核心模块
- **数据流**：详细的数据流程和交互过程
- **性能优化**：框架级别的优化技术
- **扩展性**：如何自定义和扩展框架

**适合人群**：
- 想要深入理解框架内部实现的开发者
- 需要自定义或扩展框架功能的高级用户
- 架构设计学习者

---

### 🧩 [模块详解](MODULES.md)

全面介绍 Galay 的四大模块和所有组件：

#### Algorithm 模块
- Base64、MD5、SHA256/512 加密算法
- MurmurHash3 快速哈希
- 盐值生成器

#### Common 模块
- 基础类型定义（GHandle、Host、EventType）
- 错误处理系统（CommonError、std::expected）
- 缓冲区（Buffer、RingBuffer）
- 日志系统（Logger）
- 反射和策略模式

#### Kernel 模块
- **async/**：异步 I/O（Socket、File、Timer）
- **coroutine/**：协程运行时（Coroutine、AsyncResult、CoScheduler）
- **event/**：事件循环（EventScheduler、EventEngine）
- **runtime/**：运行时管理（Runtime、ThreadPool）
- **server/**：服务器实现（TCP/UDP/SSL Server）
- **client/**：客户端实现（TCP/UDP/SSL Client）
- **time/**：定时器管理

#### Utils 模块
- 应用框架、配置解析器
- 线程池、对象池、限流器
- 熔断器、MVCC
- 堆栈跟踪、信号处理
- 字符串工具、系统信息

**适合人群**：
- 需要了解特定模块功能的开发者
- 查找 API 接口和使用方法的用户
- 想要全面了解框架能力的学习者

---

### 🔄 [协程编程指南](COROUTINE_GUIDE.md)

学习如何使用 Galay 的协程进行异步编程：

#### 基础知识
- 协程基本概念和语法
- `co_await`、`co_return` 关键字
- 协程函数的定义和调用

#### 异步操作
- **网络操作**：TCP/UDP/SSL 通信示例
- **文件操作**：异步文件读写
- **定时器**：延迟和周期性任务

#### 高级技巧
- 并发等待（AsyncWaiter）
- 超时控制
- 协程链式调用
- 协程递归

#### 最佳实践
- 错误处理模式
- 移动语义的使用
- 资源管理和生命周期
- 性能优化建议

#### 常见陷阱
- 悬空引用
- 忘记 co_await
- 数据竞争
- 资源泄漏

**适合人群**：
- 初次使用 Galay 的开发者
- 需要协程编程指导的用户
- 想要避免常见错误的学习者

---

### ⚡ [性能优化指南](PERFORMANCE.md)

掌握 Galay 的性能优化技术：

#### 性能特性
- 零拷贝设计原理
- 无锁并发实现
- 高效的事件驱动模型
- 协程和内存优化

#### 基准测试
- Echo Server 性能数据
- 与其他框架的对比
- 文件传输性能测试
- 内存分配性能测试

#### 优化策略
- **网络优化**：批量发送、缓冲区配置、Socket 选项
- **协程优化**：减少挂起、协程池
- **内存优化**：对象池、减少动态分配
- **并发优化**：线程数配置、减少锁争用

#### 性能调优
- Runtime 配置调优
- 系统级优化（文件描述符、网络参数、CPU 亲和性）
- 编译优化（编译选项、PGO）

#### 性能监控
- 内置监控实现
- 系统工具使用（perf、valgrind、strace）

#### 常见瓶颈
- 系统调用开销
- 内存分配问题
- 锁争用分析
- 缓存未命中优化

**适合人群**：
- 追求极致性能的开发者
- 需要解决性能瓶颈的用户
- 进行系统调优的运维人员

---

## 🚀 快速开始

### 1. 安装和构建

请参考项目根目录的 [README.md](../README.md) 获取安装说明。

### 2. 第一个程序

#### TCP Echo Server

```cpp
#include "galay/kernel/server/TcpServer.h"
#include "galay/kernel/runtime/Runtime.h"

using namespace galay;

Coroutine<void> handleClient(AsyncTcpSocket socket) {
    char buffer[4096];
    
    while (true) {
        auto result = co_await socket.recv(buffer, sizeof(buffer));
        
        if (!result.has_value() || result.value().empty()) {
            break;
        }
        
        co_await socket.send(std::move(result.value()));
    }
    
    co_await socket.close();
}

int main() {
    // 创建运行时
    Runtime runtime = RuntimeBuilder()
        .setCoSchedulerNum(4)
        .build();
    
    // 创建服务器
    TcpServer server(runtime);
    server.setRequestHandler(handleClient);
    server.bind({"0.0.0.0", 8080});
    server.listen();
    
    // 启动服务器
    server.start();
    
    return 0;
}
```

### 3. 更多示例

查看 `test/` 和 `benchmark/` 目录获取更多完整示例：

- `test/test_tcp_server.cc` - TCP 服务器示例
- `test/test_tcp_client.cc` - TCP 客户端示例
- `test/test_udp_server.cc` - UDP 服务器示例
- `test/test_ssl_server.cc` - SSL 服务器示例
- `benchmark/stress_tcp_server.cc` - 压力测试服务器

---

## 📖 文档阅读顺序建议

### 初学者路径

1. **快速入门** → 根目录 [README.md](../README.md)
2. **协程编程** → [协程编程指南](COROUTINE_GUIDE.md)
3. **模块功能** → [模块详解](MODULES.md)（按需查阅）

### 进阶开发者路径

1. **架构理解** → [架构设计文档](ARCHITECTURE.md)
2. **深入模块** → [模块详解](MODULES.md)
3. **性能优化** → [性能优化指南](PERFORMANCE.md)

### 高级用户路径

1. **架构设计** → [架构设计文档](ARCHITECTURE.md)
2. **性能调优** → [性能优化指南](PERFORMANCE.md)
3. **源码分析** → 结合文档阅读源代码

---

## 🔧 API 参考

详细的 API 参考请查看源代码中的内联文档注释。主要接口包括：

### 核心类

- `Runtime` / `RuntimeBuilder` - 运行时管理
- `AsyncTcpSocket` / `AsyncUdpSocket` / `AsyncSslSocket` - 网络 Socket
- `AsyncFile` - 异步文件 I/O
- `AsyncTimer` - 定时器
- `Bytes` - 字节容器
- `Buffer` - 动态缓冲区

### 服务器和客户端

- `TcpServer` / `TcpClient` - TCP 通信
- `UdpServer` / `UdpClient` - UDP 通信
- `TcpSslServer` / `TcpSslClient` - SSL/TLS 安全通信

### 工具类

- `Logger` - 日志系统
- `RateLimiter` - 限流器
- `CircuitBreaker` - 熔断器
- `Pool<T>` - 对象池
- `SignalHandler` - 信号处理

### 算法类

- `Base64Util` - Base64 编解码
- `Md5Util` / `Sha256Util` / `Sha512Util` - 哈希算法
- `Salt` - 盐值生成


## 🤝 贡献文档

我们欢迎对文档的改进建议！如果您发现：

- 文档错误或不清晰的地方
- 缺少重要信息
- 示例代码问题
- 有更好的解释方式

请通过以下方式贡献：

1. 提交 Issue 描述问题
2. 提交 Pull Request 修复文档
3. 在社区讨论中提出建议

---

## 📝 文档更新日志

### 2025-10-17

- ✅ 创建架构设计文档
- ✅ 创建模块详解文档
- ✅ 创建协程编程指南
- ✅ 创建性能优化指南
- ✅ 创建文档中心索引

---

## 📧 联系方式

- **GitHub Issues**: [项目 Issues 页面](https://github.com/your-username/galay/issues)
- **邮件**: your-email@example.com
- **讨论区**: [GitHub Discussions](https://github.com/your-username/galay/discussions)

---

## 📄 许可证

本文档与 Galay 框架采用相同的 Apache 2.0 许可证。

详见项目根目录的 [LICENSE](../LICENSE) 文件。

---

**祝您使用 Galay 开发愉快！** 🎉

