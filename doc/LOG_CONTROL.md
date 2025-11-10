# Galay 框架日志控制指南

## 概述

Galay 框架提供了灵活的日志控制系统，支持运行时动态开启/关闭日志输出，以及调整日志级别。

**默认行为**：框架日志默认**禁用**，不会产生任何日志输出，确保生产环境的性能。

## 快速开始

### 1. 启用日志

```cpp
#include "galay/common/Log.h"

int main() {
    // 启用日志，使用默认级别 debug
    galay::log::enable();
    
    // 或者指定日志级别
    galay::log::enable(spdlog::level::info);
    
    // 你的代码...
    
    return 0;
}
```

### 2. 禁用日志

```cpp
// 完全禁用日志输出
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

### 示例 2：生产环境配置

```cpp
#include "galay/kernel/runtime/Runtime.h"
#include "galay/common/Log.h"

int main() {
    // 生产环境：只输出重要信息
    galay::log::enable(spdlog::level::warn);
    
    // 或者完全禁用日志以获得最佳性能
    // galay::log::disable();
    
    galay::RuntimeBuilder builder;
    auto runtime = builder.build();
    runtime.start();
    
    // 你的业务代码...
    
    runtime.stop();
    return 0;
}
```

### 示例 3：运行时动态控制

```cpp
#include "galay/kernel/runtime/Runtime.h"
#include "galay/common/Log.h"
#include <signal.h>

// 信号处理函数：动态切换日志级别
void signalHandler(int signum) {
    if (signum == SIGUSR1) {
        // 收到 SIGUSR1 信号：启用详细日志
        galay::log::enable(spdlog::level::debug);
        std::cout << "日志已启用（debug 级别）" << std::endl;
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
    
    // 默认禁用日志
    galay::log::disable();
    
    galay::RuntimeBuilder builder;
    auto runtime = builder.build();
    runtime.start();
    
    // 运行时可以通过发送信号来控制日志：
    // kill -SIGUSR1 <pid>  # 启用日志
    // kill -SIGUSR2 <pid>  # 禁用日志
    
    // 你的业务代码...
    
    runtime.stop();
    return 0;
}
```

### 示例 4：检查日志状态

```cpp
#include "galay/common/Log.h"
#include <iostream>

int main() {
    // 检查日志是否启用
    if (galay::log::isEnabled()) {
        std::cout << "日志已启用" << std::endl;
    } else {
        std::cout << "日志已禁用" << std::endl;
    }
    
    // 获取当前日志级别
    auto level = galay::log::getLevel();
    std::cout << "当前日志级别: " << spdlog::level::to_string_view(level) << std::endl;
    
    return 0;
}
```

## API 参考

### `galay::log::enable(level)`
启用日志输出
- **参数**：`level` - 日志级别（默认为 `spdlog::level::debug`）
- **返回**：无

### `galay::log::disable()`
禁用日志输出
- **参数**：无
- **返回**：无

### `galay::log::setLevel(level)`
设置日志级别
- **参数**：`level` - 日志级别
- **返回**：无

### `galay::log::getLevel()`
获取当前日志级别
- **参数**：无
- **返回**：`spdlog::level::level_enum` - 当前日志级别

### `galay::log::isEnabled()`
检查日志是否启用
- **参数**：无
- **返回**：`bool` - true 表示已启用，false 表示已禁用

## 性能考虑

### 日志开销

- **禁用日志**（`log::disable()`）：几乎零开销，日志宏会在运行时快速返回
- **启用日志**：
  - `trace/debug` 级别：适合开发环境，有一定性能开销
  - `info` 级别：适合生产环境，开销较小
  - `warn/error` 级别：开销最小，只记录异常情况

### 最佳实践

1. **生产环境**：
   - 默认禁用日志或使用 `warn` 级别
   - 只在需要排查问题时临时启用

2. **开发环境**：
   - 使用 `debug` 或 `trace` 级别
   - 便于调试和问题定位

3. **性能测试**：
   - 完全禁用日志（`log::disable()`）
   - 获得最真实的性能数据

## 注意事项

1. **编译时开关**：
   - 框架使用 `ENABLE_SYSTEM_LOG` 宏控制日志代码的编译
   - 如果未定义该宏，所有日志代码会被完全移除（零开销）

2. **线程安全**：
   - 所有日志控制函数都是线程安全的
   - 可以在任何线程中调用

3. **日志文件**：
   - 默认日志文件路径：`logs/galay.log`
   - 支持自动滚动（默认 10MB，保留 3 个文件）

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
- ✅ 默认禁用，零性能开销
- ✅ 运行时动态控制
- ✅ 灵活的日志级别
- ✅ 简洁的 API
- ✅ 线程安全

根据你的使用场景选择合适的日志配置，在开发效率和运行性能之间取得最佳平衡。

