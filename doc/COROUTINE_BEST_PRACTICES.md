# 协程最佳实践与常见陷阱

## ⚠️ 严重警告：禁止在协程中使用阻塞操作

### 问题描述

在使用 Galay 框架的协程时，**绝对不能**在协程函数中使用任何会阻塞当前线程的同步操作。这是因为：

1. **单线程事件循环**：Galay 的事件循环运行在单个线程上
2. **协程调度依赖事件循环**：所有协程的调度、唤醒都依赖事件循环的正常运行
3. **阻塞 = 死锁**：如果协程阻塞了线程，事件循环无法运行，其他协程无法被调度

### 禁止使用的操作

#### ❌ 1. 标准库同步原语

```cpp
// ❌ 错误示例
Coroutine<nil> badExample(TcpClient client)
{
    std::mutex mtx;
    std::condition_variable cv;
    std::unique_lock<std::mutex> lock(mtx);
    
    // 这会阻塞整个事件循环线程！
    cv.wait_for(lock, std::chrono::milliseconds(100), []{ 
        return some_condition; 
    });
    
    co_return nil();
}
```

**问题**：`std::condition_variable::wait_for` 会阻塞当前线程，导致：
- 事件循环停止运行
- 其他协程无法被调度
- I/O 事件无法被处理
- 定时器无法触发

#### ❌ 2. 阻塞式 I/O

```cpp
// ❌ 错误示例
Coroutine<nil> badExample()
{
    std::string input;
    std::getline(std::cin, input);  // 阻塞整个事件循环！
    
    co_return nil();
}
```

#### ❌ 3. 阻塞式睡眠

```cpp
// ❌ 错误示例
Coroutine<nil> badExample()
{
    std::this_thread::sleep_for(std::chrono::seconds(1));  // 阻塞事件循环！
    
    co_return nil();
}
```

### 正确的替代方案

#### ✅ 1. 使用协程友好的定时器

```cpp
// ✅ 正确示例
Coroutine<nil> goodExample(CoSchedulerHandle handle)
{
    auto generator = handle.getAsyncFactory().getTimerGenerator();
    
    // 使用异步睡眠，不会阻塞事件循环
    co_await generator.sleep(std::chrono::milliseconds(1000));
    
    co_return nil();
}
```

#### ✅ 2. 使用 co_yield 让出执行权

```cpp
// ✅ 正确示例
Coroutine<nil> goodExample()
{
    while (!some_condition) {
        // 让出执行权给其他协程，不阻塞事件循环
        co_yield nil();
    }
    
    co_return nil();
}
```

#### ✅ 3. 将阻塞操作移到独立线程

```cpp
// ✅ 正确示例：输入线程 + 消息队列
std::queue<std::string> input_queue;
std::mutex input_mutex;
std::atomic_bool running{true};

// 独立的输入线程
std::thread input_thread([]() {
    while (running.load()) {
        std::string line;
        std::getline(std::cin, line);  // 在独立线程中阻塞
        
        std::lock_guard<std::mutex> lock(input_mutex);
        input_queue.push(line);
    }
});

// 协程中非阻塞地检查队列
Coroutine<nil> goodExample()
{
    while (true) {
        std::string msg;
        {
            std::lock_guard<std::mutex> lock(input_mutex);
            if (!input_queue.empty()) {
                msg = input_queue.front();
                input_queue.pop();
            }
        }
        
        if (!msg.empty()) {
            // 处理消息
        }
        
        // 让出执行权
        co_yield nil();
    }
    
    co_return nil();
}
```

## 实际案例：修复阻塞导致的死锁

### 问题场景

在测试双向通信时，发现客户端无法接收服务器发送的数据：

```cpp
// 问题代码
Coroutine<nil> testWrite(TcpClient client)
{
    while (true) {
        std::unique_lock<std::mutex> lock(input_mutex);
        
        // 这里阻塞了 100ms，导致 testRead 无法被调度！
        if (input_cv.wait_for(lock, std::chrono::milliseconds(100), []{ 
            return !input_queue.empty(); 
        })) {
            // 处理输入
        }
        
        co_yield nil();
    }
}

Coroutine<nil> testRead(TcpClient client)
{
    while (true) {
        // 即使服务器发送了数据，读事件被触发
        // 但因为 testWrite 阻塞了线程，这个协程无法被调度
        auto result = co_await client.recv(buffer, size);
        // ...
    }
}
```

**症状**：
- 服务器正常发送数据
- kqueue 正确触发读事件
- 但 `testRead` 协程无法被唤醒
- 客户端无法接收任何数据

**根本原因**：
`testWrite` 中的 `wait_for` 每 100ms 阻塞一次线程，在这期间：
1. 事件循环无法运行
2. 读事件无法被分发
3. `testRead` 协程无法被唤醒

### 解决方案

```cpp
// 修复后的代码
Coroutine<nil> testWrite(TcpClient client)
{
    auto generator = handle.getAsyncFactory().getTimerGenerator();
    
    // 发送消息
    co_await client.send(Bytes::fromString("start"));
    
    // 使用异步睡眠代替阻塞等待
    co_await generator.sleep(std::chrono::milliseconds(5000));
    
    co_await client.send(Bytes::fromString("quit"));
    
    co_return nil();
}

Coroutine<nil> testRead(TcpClient client)
{
    while (true) {
        // 现在可以正常接收数据了！
        auto result = co_await client.recv(buffer, size);
        if (result) {
            std::cout << "Received: " << result.value().toString() << std::endl;
        }
        co_yield nil();
    }
}
```

**结果**：
- 客户端成功接收服务器发送的所有消息
- 双向通信正常工作
- 事件循环流畅运行

## 调试技巧

### 1. 识别阻塞问题

如果遇到以下症状，很可能是协程中使用了阻塞操作：

- ✗ 事件明明被触发了，但协程没有响应
- ✗ 定时器不准确或延迟很大
- ✗ 某些协程"卡住"不执行
- ✗ CPU 使用率异常（过高或过低）

### 2. 检查代码

搜索以下关键字，检查是否在协程中使用：

```bash
# 危险的阻塞操作
grep -r "wait_for" your_coroutine_code.cc
grep -r "wait_until" your_coroutine_code.cc
grep -r "sleep_for" your_coroutine_code.cc
grep -r "sleep_until" your_coroutine_code.cc
grep -r "getline" your_coroutine_code.cc
grep -r "cin >>" your_coroutine_code.cc
```

### 3. 添加日志

在关键位置添加日志，追踪协程的执行流程：

```cpp
Coroutine<nil> debugExample()
{
    std::cout << "Coroutine started" << std::endl;
    
    co_await some_async_operation();
    std::cout << "After async operation" << std::endl;  // 如果没打印，说明协程被阻塞
    
    co_return nil();
}
```

## 总结

### 核心原则

> **在协程中，永远不要阻塞线程！**

### 记住这些规则

1. ✅ 使用 `co_await` 进行异步等待
2. ✅ 使用 `co_yield` 让出执行权
3. ✅ 使用 `TimerGenerator::sleep()` 代替 `std::this_thread::sleep_for()`
4. ✅ 将阻塞操作移到独立线程
5. ❌ 不要使用 `std::condition_variable::wait*`
6. ❌ 不要使用 `std::this_thread::sleep*`
7. ❌ 不要使用阻塞式 I/O（如 `std::cin`、`std::getline`）
8. ❌ 不要使用任何会阻塞当前线程的操作

### 设计建议

- **输入处理**：使用独立线程 + 消息队列
- **定时操作**：使用 `TimerGenerator`
- **等待条件**：使用 `co_yield` + 轮询，或设计基于事件的通知机制
- **长时间计算**：考虑移到线程池执行，通过回调通知协程

遵循这些原则，你的异步程序将运行流畅、响应迅速！

