# AsyncQueue 实现问题分析

## 问题1：🔴 Lost Wakeup - 致命竞态条件

### 代码
```cpp
AsyncResult<T> waitDequeue() {
    T out;
    if(m_queue.try_dequeue(out)) {
        return AsyncResult<T>(std::move(out));
    }
    return m_waiter.wait();  // ← 如果队列为空，等待
}

void enqueue(T&& value) {
    m_queue.push(std::move(value));
    m_waiter.notify({});
}
```

### 致命竞态场景

```
时间    协程A (waitDequeue)           协程B (enqueue)
t1      try_dequeue() = false
        [检查完毕，队列为空]

t2                                    push(value) 入队
                                      notify()  通知等待者
                                      [通知已发送！]

t3      m_waiter.wait()               [太晚了！]
        [等待通知，但通知已经发过了]

结果：
✓ 数据在队列中
✗ 通知已经发送过
✗ 协程A 永久等待，从不被唤醒
✗ 死锁！
```

### 为什么会死锁？

AsyncWaiter 的设计是"等待通知"模式：
```cpp
notify() -> 标记有消息
wait()   -> 检查是否有消息
            如果没有，就暂停等待下次 notify()
```

但 waitDequeue 的问题是：
- 检查队列（没有数据）
- 然后才调用 wait()
- 但在这两个操作之间，enqueue/notify 可能已经发生过

### 这是 AsyncQueue 的**致命设计缺陷**

---

## 问题2：🔴 多协程等待时，只有第一个被唤醒

### 场景

```
协程A: waitDequeue()  [等待]
协程B: waitDequeue()  [等待]
协程C: enqueue(value) + notify()
```

假设 notify() 只唤醒一个协程，那么：
- 协程A 被唤醒，从队列取走 value
- 协程B 仍然在等待
- 但队列中没有更多数据了
- 协程B 永久等待！

### 需要验证

AsyncWaiter::notify() 是否会唤醒所有等待者，还是只唤醒一个？

从之前看过的代码：
```cpp
bool AsyncWaiter::notify(std::expected<T, E> &&value)
{
    bool expected = true;
    if(m_wait.compare_exchange_strong(expected, false,
                                  std::memory_order_acq_rel,
                                  std::memory_order_acquire)) {
        m_event->m_result = std::move(value);
        m_waker.wakeUp();  // ← 只唤醒一个！
        return true;
    }
    return false;
}
```

**确实，只唤醒一个！** 这对于 AsyncQueue 多消费者场景是个问题。

---

## 问题3：⚠️ 竞态条件：enqueue 和 notify 之间

### 代码
```cpp
void enqueue(T&& value) {
    m_queue.push(std::move(value));      // Step 1
    m_waiter.notify({});                  // Step 2
}
```

### 场景
```
协程A (waitDequeue):           协程B (enqueue):
try_dequeue()                  push(value)  [Step 1 完成]
  -> 失败（此时队列可能有数据！）   notify()  [Step 2 进行中]
wait()
  -> 等待
                               notify() 完成
```

**不过，这个场景中 Step 1 先于 waitDequeue 的 try_dequeue，所以应该没问题...但时序很微妙。**

---

## 问题4：⚠️ size() 和 empty() 的 TOCTOU

### 代码
```cpp
size_t size() const { return m_queue.size(); }
bool empty() const { return m_queue.empty(); }
```

### 问题
```cpp
// 使用者代码
if (!queue.empty()) {
    auto result = co_await queue.waitDequeue();  // 但中间可能队列被其他协程取空
}
```

这是 Time-of-Check-Time-of-Use (TOCTOU) 问题，虽然在 AsyncQueue 本身不是bug，但容易误导使用者。

---

## 问题5：⚠️ 对象生命周期

### 代码中没有看到的问题

AsyncWaiter 是内部成员，当 AsyncQueue 销毁时会自动销毁。但如果有协程还在 wait() 呢？

Waker 持有对协程的 weak_ptr，所以应该是安全的。但需要验证。

---

## 真正的修复：需要 Double-Check 模式！

### 正确的实现应该是

```cpp
AsyncResult<T> waitDequeue() {
    T out;

    // Double-Check 模式（类似 AsyncMutex 的 onSuspend）
    if(m_queue.try_dequeue(out)) {
        return AsyncResult<T>(std::move(out));
    }

    // 关键：必须保证在调用 wait() 前，后续的 enqueue 能被看到
    // 这需要 notify() 是有状态的，或者 wait() 本身检查队列

    return m_waiter.wait();
}
```

但这还是有问题，因为：
1. notify() 在 push 之前调用
2. wait() 在 try_dequeue 失败后调用
3. 中间有 gap

### 真正的解决方案

参考 AsyncMutex 的做法，但针对队列：

```cpp
AsyncResult<T> waitDequeue() {
    // Step 1: 标记"我要等待"
    m_waiter.markWaiting();  // 原子操作

    // Step 2: Double-Check 队列
    T out;
    if(m_queue.try_dequeue(out)) {
        m_waiter.cancelWaiting();  // 取消等待标记
        return AsyncResult<T>(std::move(out));
    }

    // Step 3: 真的进入等待
    return m_waiter.wait();
}

void enqueue(T&& value) {
    m_queue.push(std::move(value));

    // 如果有人在等待，唤醒
    // 如果没人等待，标记"有数据"
    m_waiter.notifyOrMark();
}
```

---

## 问题总结表

| 问题 | 严重度 | 影响 | 证据 |
|------|--------|------|------|
| **Lost Wakeup** | 🔴 致命 | 永久死锁 | try_dequeue 到 wait() 的 gap |
| **单唤醒** | 🔴 致命 | 多消费者饥荒 | notify() 只调用一次 wakeUp() |
| **时序依赖** | 🟡 严重 | 竞态条件 | push/notify 的顺序 |
| **TOCTOU** | 🟠 轻 | 使用难度 | size()/empty() |
| **多等待者** | 🔴 致命 | 只有第一个被唤醒 | AsyncWaiter 设计 |

---

## 对比 AsyncMutex

### AsyncMutex 为什么安全，AsyncQueue 为什么不安全？

**AsyncMutex**：
```cpp
bool LockEvent::onSuspend(Waker waker)
{
    // Step 1: 入队
    m_waiter.push(waker);

    // Step 2: Double-Check 再获取一次
    if (m_mutex.tryLock()) {
        pop();  // 从队列移除自己
        return false;  // 不等待
    }

    // Step 3: 真的要等待了，但已经在队列中
    return true;
}
```

关键：**Double-Check + 入队的原子性保证了不会丢失唤醒**。

**AsyncQueue**：
```cpp
AsyncResult<T> waitDequeue() {
    T out;
    if(m_queue.try_dequeue(out)) {  // Check
        return AsyncResult<T>(std::move(out));
    }
    return m_waiter.wait();         // 然后 Use
}
```

关键缺陷：**Check 和 Use 之间没有同步，导致可能在 enqueue 已发生后才 wait()**。

---

## 建议的修复方向

### 方案1：类似 AsyncMutex 的设计

```cpp
class AsyncQueue {
private:
    AsyncWaiter<T, Error> m_waiter;
    ConcurrentQueue<T> m_queue;
    std::atomic<bool> m_isWaiting = false;  // ← 关键
};

AsyncResult<T> waitDequeue() {
    T out;

    // Step 1: 标记"我要等待"
    bool expected = false;
    m_isWaiting.compare_exchange_strong(expected, true);

    // Step 2: Double-Check
    if(m_queue.try_dequeue(out)) {
        m_isWaiting.store(false);
        return AsyncResult<T>(std::move(out));
    }

    // Step 3: 真的等待
    return m_waiter.wait();
}

void enqueue(T&& value) {
    m_queue.push(std::move(value));

    if (m_isWaiting) {
        m_waiter.notify(value);
    }
}
```

### 方案2：改进 AsyncWaiter

AsyncWaiter 本身需要支持"标记有数据"而不是"通知等待者"。

---

## 结论

AsyncQueue 的实现有**多个致命缺陷**，特别是：

1. **Lost Wakeup** - try_dequeue 到 wait() 的竞态窗口
2. **单协程唤醒** - notify() 只唤醒一个，多消费者场景死锁
3. **设计模式不当** - 缺少 Double-Check

**建议立即修复，否则在多协程竞争场景会出现死锁或数据丢失！**

