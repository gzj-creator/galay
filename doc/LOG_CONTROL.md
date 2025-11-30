# Galay 框架日志控制指南

## 概述

Galay 框架提供了灵活的日志控制系统，支持运行时动态开启/关闭日志输出，以及调整日志级别。

**默认行为**：
- 框架日志默认**完全禁用**
- **不会创建日志文件**，直到首次调用 `galay::log::enable()`
- 真正的零开销，确保生产环境的最佳性能

## 延迟初始化特性

日志系统采用**延迟初始化**策略：
- ✅ 默认状态下不会创建任何日志文件或目录
- ✅ 只有在首次调用 `enable()` 时才会初始化日志系统
- ✅ 未初始化时，所有日志宏调用都是安全的空操作
- ✅ 避免不必要的文件系统操作和资源占用

## 快速开始

### 1. 启用日志

```cpp
#include "galay/common/Log.h"

int main() {
    // 启用日志，使用默认级别 debug（首次调用会创建日志文件）
    galay::log::enable();
    
    // 或者指定日志级别
    galay::log::enable(spdlog::level::info);
    
    // 你的代码...
    
    return 0;
}
```

### 2. 禁用日志

```cpp
// 完全禁用日志输出（日志文件保留，但不再写入）
galay::log::disable();
```

### 3. 动态调整日志级别

```cpp
// 设置为 info 级别（只输出 info、warn、error、critical）
galay::log::setLevel(spdlog::level::info);

// 设置为 error 级别（只输出 error 和 critical）
galay::log::setLevel(spdlog::level::err);

// 设置为 trace 级别（输出所有日志）
galay::log::setLevel(spdlog::level::trace);
```

### 4. 检查日志状态

```cpp
// 检查日志是否启用
if (galay::log::isEnabled()) {
    std::cout << "日志已启用" << std::endl;
}

// 检查日志系统是否已初始化
if (galay::log::isInitialized()) {
    std::cout << "日志系统已初始化" << std::endl;
}

// 获取当前日志级别
auto level = galay::log::getLevel();
```

## 日志级别说明

从低到高的日志级别：

| 级别 | 说明 | 使用场景 |
|------|------|----------|
| `trace` | 最详细的跟踪信息 | 深度调试 |
| `debug` | 调试信息 | 开发和调试 |
| `info` | 一般信息 | 生产环境的关键信息 |
| `warn` | 警告信息 | 潜在问题 |
| `err` | 错误信息 | 错误但不致命 |
| `critical` | 严重错误 | 致命错误 |
| `off` | 关闭日志 | 生产环境禁用日志 |

## 完整示例

### 示例 1：开发环境配置

```cpp
#include "galay/kernel/runtime/Runtime.h"
#include "galay/common/Log.h"

int main() {
    // 开发环境：启用详细日志
    galay::log::enable(spdlog::level::debug);
    
    galay::RuntimeBuilder builder;
    auto runtime = builder.build();
    runtime.start();
    
    // 你的业务代码...
    
    runtime.stop();
    return 0;
}
```

### 示例 2：生产环境配置（默认不创建日志）

```cpp
#include "galay/kernel/runtime/Runtime.h"
#include "galay/common/Log.h"

int main() {
    // 生产环境：默认不启用日志，不会创建任何日志文件
    // 无需调用任何日志配置函数
    
    galay::RuntimeBuilder builder;
    auto runtime = builder.build();
    runtime.start();
    
    // 你的业务代码...
    // 日志宏调用是安全的，但不会有任何输出
    
    runtime.stop();
    return 0;
}
```

### 示例 3：按需启用日志

```cpp
#include "galay/kernel/runtime/Runtime.h"
#include "galay/common/Log.h"

int main() {
    // 默认不启用日志
    
    galay::RuntimeBuilder builder;
    auto runtime = builder.build();
    runtime.start();
    
    // 运行一段时间后，如果需要调试，可以动态启用日志
    if (need_debugging) {
        galay::log::enable(spdlog::level::debug);
        // 从此刻开始，日志文件会被创建并开始记录
    }
    
    // 你的业务代码...
    
    runtime.stop();
    return 0;
}
```

### 示例 4：信号控制日志（运行时动态切换）

```cpp
#include "galay/kernel/runtime/Runtime.h"
#include "galay/common/Log.h"
#include <signal.h>

// 信号处理函数：动态切换日志级别
void signalHandler(int signum) {
    if (signum == SIGUSR1) {
        // 收到 SIGUSR1 信号：启用详细日志
        if (!galay::log::isInitialized()) {
            galay::log::enable(spdlog::level::debug);
            std::cout << "日志已启用（debug 级别）" << std::endl;
        } else {
            galay::log::setLevel(spdlog::level::debug);
            std::cout << "日志级别已调整为 debug" << std::endl;
        }
    } else if (signum == SIGUSR2) {
        // 收到 SIGUSR2 信号：禁用日志
        galay::log::disable();
        std::cout << "日志已禁用" << std::endl;
    }
}

int main() {
    // 注册信号处理
    signal(SIGUSR1, signalHandler);
    signal(SIGUSR2, signalHandler);
    
    // 默认不启用日志（不创建文件）
    
    galay::RuntimeBuilder builder;
    auto runtime = builder.build();
    runtime.start();
    
    // 运行时可以通过发送信号来控制日志：
    // kill -SIGUSR1 <pid>  # 启用日志（首次会创建文件）
    // kill -SIGUSR2 <pid>  # 禁用日志
    
    // 你的业务代码...
    
    runtime.stop();
    return 0;
}
```

### 示例 5：条件性日志初始化

```cpp
#include "galay/common/Log.h"
#include <iostream>

int main(int argc, char* argv[]) {
    // 根据命令行参数决定是否启用日志
    bool enable_logging = false;
    
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--enable-log") {
            enable_logging = true;
            break;
        }
    }
    
    if (enable_logging) {
        galay::log::enable(spdlog::level::info);
        std::cout << "日志已启用" << std::endl;
    } else {
        std::cout << "日志未启用（不会创建日志文件）" << std::endl;
    }
    
    // 你的代码...
    
    return 0;
}
```

## API 参考

### `galay::log::enable(level)`
启用日志输出
- **参数**：`level` - 日志级别（默认为 `spdlog::level::debug`）
- **返回**：无
- **说明**：首次调用时会初始化日志系统并创建日志文件

### `galay::log::disable()`
禁用日志输出
- **参数**：无
- **返回**：无
- **说明**：禁用后日志系统仍然存在，只是不输出日志

### `galay::log::setLevel(level)`
设置日志级别
- **参数**：`level` - 日志级别
- **返回**：无
- **说明**：如果日志系统未初始化，不会触发初始化

### `galay::log::getLevel()`
获取当前日志级别
- **参数**：无
- **返回**：`spdlog::level::level_enum` - 当前日志级别
- **说明**：未初始化时返回 `spdlog::level::off`

### `galay::log::isEnabled()`
检查日志是否启用
- **参数**：无
- **返回**：`bool` - true 表示已启用，false 表示已禁用
- **说明**：未初始化时返回 false

### `galay::log::isInitialized()`
检查日志系统是否已初始化
- **参数**：无
- **返回**：`bool` - true 表示已初始化，false 表示未初始化
- **说明**：用于判断日志文件是否已创建

## 性能考虑

### 日志开销

- **未初始化状态**：**真正的零开销**
  - 不创建任何文件或目录
  - 不分配任何日志相关资源
  - 日志宏调用会被快速跳过（空指针检查）
  
- **禁用状态**（已初始化）：**极低开销**
  - 日志文件已创建但不写入
  - 日志宏会在 spdlog 层面被过滤
  
- **启用状态**：
  - `trace/debug` 级别：适合开发环境，有一定性能开销
  - `info` 级别：适合生产环境，开销较小
  - `warn/error` 级别：开销最小，只记录异常情况

### 最佳实践

1. **生产环境**：
   - 默认不启用日志（不调用 `enable()`）
   - 完全避免日志文件创建和磁盘 I/O
   - 只在需要排查问题时通过信号或配置动态启用

2. **开发环境**：
   - 使用 `debug` 或 `trace` 级别
   - 便于调试和问题定位

3. **性能测试**：
   - 确保日志未初始化（不调用 `enable()`）
   - 获得最真实的性能数据

4. **条件编译**：
   - 框架使用 `ENABLE_SYSTEM_LOG` 宏控制日志代码的编译
   - 如果未定义该宏，所有日志代码会被完全移除（编译时零开销）

## 日志文件管理

### 默认配置

- **日志文件路径**：`logs/galay.log`
- **日志滚动策略**：按大小滚动
- **单文件最大大小**：10MB
- **保留文件数**：3 个
- **日志格式**：`[时间][级别][线程ID][文件:行号][函数名] 消息`

### 自定义日志配置

如果需要自定义日志配置，可以使用底层 API：

```cpp
// 创建自定义日志记录器
auto custom_logger = galay::Logger::createRotingFileLoggerMT(
    "my_logger",
    "my_logs/app.log",
    20 * 1024 * 1024,  // 20MB
    5                   // 保留5个文件
);

// 设置自定义格式
custom_logger->pattern("[%Y-%m-%d %H:%M:%S] [%l] %v");

// 设置日志级别
custom_logger->level(spdlog::level::info);

// 替换默认日志记录器
galay::details::InternelLogger::getInstance()->setLogger(std::move(custom_logger));
```

## 注意事项

1. **延迟初始化**：
   - 日志系统采用延迟初始化，首次调用 `enable()` 时才会创建日志文件
   - 在 `enable()` 之前的所有日志调用都是安全的空操作

2. **线程安全**：
   - 所有日志控制函数都是线程安全的
   - 可以在任何线程中调用

3. **异步日志**：
   - 日志系统使用异步写入，不会阻塞业务线程
   - 程序退出前建议调用 `shutdown()` 确保所有日志被刷新

4. **编译时开关**：
   - 框架使用 `ENABLE_SYSTEM_LOG` 宏控制日志代码的编译
   - 如果未定义该宏，所有日志代码会被完全移除

## 旧版本兼容

如果你的代码使用了旧的日志控制方式：

```cpp
// 旧方式（仍然支持）
galay::details::InternelLogger::getInstance()->setLevel(spdlog::level::debug);

// 新方式（推荐）
galay::log::enable(spdlog::level::debug);
```

两种方式都可以使用，但推荐使用新的 `galay::log::*` API，更简洁易用。

## 总结

Galay 框架的日志系统提供了：
- ✅ 默认完全禁用，不创建任何文件
- ✅ 延迟初始化，按需加载
- ✅ 运行时动态控制
- ✅ 灵活的日志级别
- ✅ 简洁的 API
- ✅ 线程安全
- ✅ 真正的零开销（未初始化时）

根据你的使用场景选择合适的日志配置，在开发效率和运行性能之间取得最佳平衡。

