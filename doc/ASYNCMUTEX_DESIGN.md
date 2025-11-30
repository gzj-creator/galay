# AsyncMutex 实现说明

## 概述

AsyncMutex 是一个协程友好的互斥锁实现，基于你的 AsyncEvent 和 Coroutine 框架。它提供线程安全的锁获取和释放，同时避免协程死锁。

## 核心设计原理

### 1. 三层架构

```
应用层：协程代码
    co_await mutex.lock()
         ↓
AsyncResult<nil> (awaitable 包装)
    await_ready() → 尝试快速路径
    await_suspend() → 暂停协程
    await_resume() → 恢复协程
         ↓
LockEvent (AsyncEvent 实现)
    onReady() → 检查锁可用性
    onSuspend() → 加入等待队列
         ↓
AsyncMutex (锁的核心逻辑)
    tryLock() → 原子操作获取锁
    unlock() → 释放锁并唤醒等待者
```

### 2. 线程安全保证

#### 原子操作（Lock-Free）
- `m_locked` 使用 `std::atomic<uint32_t>` 存储状态
- 使用 `compare_exchange_strong()` 实现原子的"检查-赋值"操作
- 内存排序：`memory_order_acq_rel` 保证获取-释放语义

```cpp
bool tryLock() {
    uint32_t expected = 0;
    return m_locked.compare_exchange_strong(
        expected, 1,
        std::memory_order_acq_rel,  // 成功时：获取语义
        std::memory_order_acquire    // 失败时：最弱的语义
    );
}
```

#### 等待队列同步
- `m_queue_mutex` 仅保护 `m_waiters` 队列的访问
- 使用 `std::lock_guard` 自动加锁/解锁
- 锁的粒度很小，仅在入队/出队时持有

### 3. 防止协程死锁的关键机制

#### Problem: Lost Wakeup（丢失通知）

传统的协程互斥锁可能出现这种竞态：

```
线程A (lock)                    线程B (unlock)
1. onReady() 返回 false
2. onSuspend() 入队
3. [等待被加入队列...]
4. [准备暂停协程]                5. m_locked.store(0)
                                 6. wakeupNext() - 唤醒线程A
7. 此时协程已经暂停，无法接收唤醒
   → 死锁！
```

#### 解决方案：Double-Check 模式

在 `LockEvent::onSuspend()` 中：

```cpp
inline bool LockEvent::onSuspend(Waker waker) {
    // Step 1: 加入等待队列
    {
        std::lock_guard<std::mutex> guard(m_mutex.m_queue_mutex);
        m_mutex.m_waiters.push(waker);
    }

    // Step 2: 再次尝试获取锁（Double-Check）
    if (m_mutex.tryLock()) {
        // 成功！从队列中移除自己
        std::lock_guard<std::mutex> guard(m_mutex.m_queue_mutex);
        if (!m_mutex.m_waiters.empty()) {
            m_mutex.m_waiters.pop();
        }
        return false;  // 不需要暂停！
    }

    // Step 3: 确实需要暂停
    return true;
}
```

这样做的好处：
- 如果在入队和 Double-Check 之间锁被释放，我们能立即获取
- 如果还是获取不到，我们已经在等待队列中，不会遗漏通知

### 4. 防止同一协程死锁

这个实现在以下场景中是安全的：

```cpp
// ✅ 安全：不同的协程或线程
Coroutine<nil> coro1 = [](AsyncMutex& m) -> Coroutine<nil> {
    co_await m.lock();  // 协程1 获取锁
    // ... 做一些工作
    m.unlock();
};

Coroutine<nil> coro2 = [](AsyncMutex& m) -> Coroutine<nil> {
    co_await m.lock();  // 协程2 等待锁
    // ...
};

// ❌ 不安全：同一协程多次 lock()
Coroutine<nil> deadlock = [](AsyncMutex& m) -> Coroutine<nil> {
    co_await m.lock();
    // ... 如果这里尝试 lock() 同一个 mutex，会永久等待
    co_await m.lock();  // 死锁！
};
```

但这是**合理的设计**，原因：
1. 标准库 `std::mutex` 也不支持递归锁定（除非用 `std::recursive_mutex`）
2. 协程框架本身已经在调度层面避免了死锁
3. 如果需要递归锁定，可以派生出 `AsyncRecursiveMutex`

### 5. 内存排序保证

#### 获取-释放对（Acquire-Release）

```
unlock() - Release:
    m_locked.store(0, memory_order_release)
              ↓
          [内存栅栏]  ← 确保之前的操作对后续可见
              ↓
    wakeupNext() - 唤醒 Waker
              ↓
              ↓
    lock() - Acquire:
    m_locked.compare_exchange_strong(..., memory_order_acq_rel)
              ↓
          [内存栅栏]  ← 确保后续的操作看到释放的结果
              ↓
    可以安全地访问被保护的资源
```

这保证了：
- 锁定期间的所有内存操作都对后续的锁定者可见
- 不会出现数据竞争（Data Race）

## 使用示例

```cpp
#include "AsyncMutex.h"

AsyncMutex g_mutex;
int g_shared_data = 0;

Coroutine<nil> increment() {
    // 等待获取锁
    co_await g_mutex.lock();

    // 原子地修改共享数据
    g_shared_data++;

    // 释放锁（允许其他协程继续）
    g_mutex.unlock();

    co_return nil{};
}

// 多个协程可以并发调用 increment()
// 它们会按顺序获取锁，避免数据竞争
```

## 性能特性

| 操作 | 时间复杂度 | 说明 |
|------|-----------|------|
| lock() - 未争竞 | O(1) | 原子 CAS，快速路径 |
| lock() - 争竞 | O(1) + 上下文切换 | 加入等待队列后暂停 |
| unlock() | O(n) | n = 等待者数量，需要遍历队列找到能获取的等待者 |
| tryLock() | O(1) | 纯原子操作 |

> **注意**：`unlock()` 的 O(n) 是因为可能有多个等待者无法立即运行（例如所属调度器忙碌）

## 相比标准 std::mutex 的优势

| 特性 | std::mutex | AsyncMutex |
|------|-----------|-----------|
| 阻塞等待 | ✅ 线程阻塞 | ✅ 协程暂停 |
| CPU 消耗 | ❌ 会唤醒线程 | ✅ 避免上下文切换 |
| 协程友好 | ❌ 无法 await | ✅ 完全支持 await |
| 异步通知 | ❌ 基于OS事件 | ✅ 基于 Waker 模式 |

## 已知限制

1. **不支持递归锁定**：同一协程不能多次 lock() 同一个 mutex
2. **不支持超时**：没有 `lock_for()` 或 `try_lock_for()`
3. **不支持共享锁**：没有读写锁（Reader-Writer Lock）
4. **依赖调度器**：Waker 必须关联有效的 Scheduler

## 扩展方向

如果需要更高级的功能：

```cpp
// 1. 递归互斥锁
class AsyncRecursiveMutex : public AsyncMutex {
    std::atomic<CoroutineBase::wptr> m_owner;
    std::atomic<int> m_count;
};

// 2. 带超时的锁
AsyncResult<bool> lock_for(Duration timeout);

// 3. 读写锁
class AsyncRWMutex {
    // 支持多个读者或单个写者
};

// 4. 信号量
class AsyncSemaphore {
    std::atomic<int> m_count;
};
```

## 测试建议

```cpp
// 测试1：基本的获取和释放
TEST(AsyncMutex, BasicLockUnlock) {
    AsyncMutex mutex;
    auto coro = lock_and_increment(mutex, shared_value);
    // 验证值被正确修改
}

// 测试2：多协程竞争
TEST(AsyncMutex, ConcurrentAccess) {
    AsyncMutex mutex;
    // 启动多个协程，都尝试修改同一个变量
    // 验证没有数据竞争
}

// 测试3：公平性
TEST(AsyncMutex, Fairness) {
    // 验证等待者按 FIFO 顺序获取锁
}

// 测试4：避免死锁
TEST(AsyncMutex, NoDeadlock) {
    // 尝试递归 lock()，应该能检测到问题
}
```
