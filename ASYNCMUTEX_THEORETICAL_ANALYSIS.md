# AsyncMutex 理论分析：合理性与安全性

## 1. 设计架构回顾

```
lock() 调用
  ↓
onReady() - tryLock() 快速路径 (成功 → 直接返回)
  ↓ (失败)
onSuspend() - 入队 + Double-Check + 暂停
  ↓
协程暂停等待

unlock() 调用
  ↓
store(0, release) - 释放锁
  ↓
wakeupNext() - 从队列唤醒
  ↓
协程恢复执行
```

## 2. 理论问题分析

### 问题1：onSuspend 中的 pop 逻辑不准确

**代码**：
```cpp
bool LockEvent::onSuspend(Waker waker)
{
    // Step 1: 入队
    {
        std::lock_guard<std::mutex> guard(m_mutex.m_queue_mutex);
        m_mutex.m_waiters.push(waker);  // 入队 "this"
    }

    // Step 2: Double-Check
    if (m_mutex.tryLock())
    {
        std::lock_guard<std::mutex> guard(m_mutex.m_queue_mutex);
        if (!m_mutex.m_waiters.empty())
        {
            m_mutex.m_waiters.pop();  // ❌ 问题！只 pop 了首元素
        }
        return false;
    }

    return true;  // ✓ 入队的是 "this"，后续由 wakeupNext() 处理
}
```

**问题**：
- 如果队列有多个 waker，我们只是 pop 了首元素
- 但我们不能确认被 pop 的就是当前的 waker
- 实际上，由于是 FIFO 队列，最后入队的会被最后 pop

**具体场景**：
```
时间    线程/协程                 队列状态
t1      协程A onSuspend()        []
t2      协程A push(A)            [A]
t3      协程A tryLock()=失败
t4      协程A return true        [A]
                                  (协程A 暂停)

t5      协程B onSuspend()        [A]
t6      协程B push(B)            [A, B]
t7      协程B Double-Check tryLock()=成功
t8      协程B pop()              [A]  <- 弹出的是A，不是B！
t9      协程B return false       [A]
                                  (协程B 立即继续)

问题：协程B 已经获取了锁，但队列中还有等待的协程A！
```

**安全性影响**：🔴 **严重**
- 协程B 虽然获取了锁，但如果它调用 unlock()
- wakeupNext() 会唤醒协程A
- 协程A 会发现自己其实已经获取了锁（因为 B 没有再次 tryLock）

**实际上等等...**，让我重新看逻辑：

```cpp
if (m_mutex.tryLock())  // 成功
{
    // 协程获取了锁！
    pop();  // 移除自己
    return false;  // 不暂停
}
return true;  // 暂停（此时锁仍在 m_locked=0，因为 tryLock 失败了）
```

不对，如果 tryLock() 失败，m_locked 仍然是 1（被其他线程持有）。所以 return true 暂停时，锁确实被占用着。

### 问题2：wakeupNext() 中的竞争

**代码**：
```cpp
void AsyncMutex::wakeupNext()
{
    std::lock_guard<std::mutex> guard(m_queue_mutex);

    while (!m_waiters.empty())
    {
        Waker waker = m_waiters.front();
        m_waiters.pop();

        // ❌ 问题：此时 m_locked 已经是 0
        // 但我们要 tryLock() 以确保只有一个协程被唤醒
        if (tryLock())
        {
            waker.wakeUp();
            return;  // ✓ 唤醒一个就返回
        }
        // ❌ 如果 tryLock 失败，说明有另一个线程/协程拿到了锁
        // 这可能意味着有多个线程在调用 unlock！
    }
}
```

**竞争场景**：
```
线程1: unlock()              线程2: unlock()
  store(0, release)
  lock(queue_mutex)          （等待）
  pop(waker1)
  tryLock()=成功             （获得 queue_mutex）
  waker1.wakeUp()            pop(waker2)
  unlock(queue_mutex)        tryLock()=失败！（因为线程1已经 tryLock 成功）
                             继续循环
                             pop(waker3)
                             tryLock()=成功
                             waker3.wakeUp()
                             unlock(queue_mutex)
```

**问题**：
- 如果有多个线程同时调用 unlock()，会发生竞争
- 但代码本身没有禁止多线程调用 unlock()
- 虽然通常 unlock() 应该由持有锁的线程调用...

**但等等**，AsyncMutex 是协程互斥锁，不是线程锁！

### 问题3：协程 vs 线程的混淆

**关键认识**：
- AsyncMutex 用于同步"协程"，不是"线程"
- 多个协程运行在同一个线程上（由调度器负责调度）
- 所以不存在"多线程同时调用 unlock()"的情况

**正确的使用场景**：
```cpp
Coroutine<nil> coro1 = [](AsyncMutex& m) -> Coroutine<nil> {
    co_await m.lock();
    // 临界区
    m.unlock();
    co_return nil{};
};

Coroutine<nil> coro2 = [](AsyncMutex& m) -> Coroutine<nil> {
    co_await m.lock();
    // 临界区
    m.unlock();
    co_return nil{};
};
```

两个协程都在同一个线程（同一个调度器）中运行，所以：
- **不会有多线程竞争**
- 但 **unlock() 会暂停当前协程，让其他协程继续运行**

### 问题4：暂停与唤醒的时序

**关键问题**：onSuspend() 返回 true 后，协程才会真正暂停。但 Waker 已经入队了。如果在暂停前 unlock() 被调用呢？

```
时间    onSuspend                    unlock
t1      入队(Waker)
t2      Double-Check tryLock=失败
t3      返回 true
                                    t4      store(0)
                                    t5      wakeupNext() {
                                               pop(Waker)
                                               tryLock()=成功
                                               wakeUp()  <- 唤醒了一个还没暂停的协程！
                                             }
t6      await_suspend {
          become(Suspended)  <- 标记为暂停
          return true
        }
```

**问题**：
- Waker 在协程暂停前被唤醒了
- 这能处理吗？

**答案**：取决于 Waker::wakeUp() 的实现。如果它是"标记协程为就绪，让调度器运行"，那么：
- 即使协程还没暂停，也只是被标记为就绪
- 这对调度器来说是幂等的（idempotent）
- 多次调用也没关系

## 3. 内存顺序分析

### 使用的内存顺序

**lock()**：
```cpp
m_locked.compare_exchange_strong(
    expected, 1,
    std::memory_order_acq_rel,  // 成功：acq_rel
    std::memory_order_acquire   // 失败：acquire
);
```

**unlock()**：
```cpp
m_locked.store(0, std::memory_order_release);
```

### 语义分析

**假设场景**：
```
线程A (lock 后的临界区)        线程B (unlock)
lock() - acquire
  ↓
读 x = 5
修改 x = 6
unlock() - release
                             unlock() - release
                             m_locked.store(0)
                             ↓
                             lock() - acquire
                             读 x = ? (应该是 6)
```

**内存顺序保证**：
- A 的 release 对 B 的 acquire 形成"同步边界"
- A 的所有内存操作都对 B 可见

**但问题**：如果有多个线程呢？

```
线程A: store(x=6) -> lock() acquire
线程B: lock() acquire -> store(x=7)
线程C: lock() acquire -> read x (期望 6 或 7)
```

acq_rel 的语义是"获取时看到所有先前的释放"，但**不保证线程间的全局顺序**。

### 评价

**✓ 足够吗**？

如果只有"释放方"和"获取方"两种角色，acq_rel 足够。

但如果有多个线程都在做 lock/unlock，可能需要更强的保证（比如 seq_cst）。

**但实际上**，这里的设计是针对协程的，不是针对线程的。协程都在同一个线程上，所以不存在"多线程"问题。

## 4. 数据竞争分析

### counter++ 的竞争

```cpp
co_await state->mutex.lock();
{
    int old_val = state->counter;      // 读
    state->counter = old_val + 1;      // 写

    if (state->counter != old_val + 1) {  // 验证
        state->errors++;
    }
}
state->mutex.unlock();
```

**问题**：如果 lock() 没有真正保护临界区呢？

```
协程A                              协程B
lock() acquire
  ↓
old_val = counter (5)
                                   lock() acquire (应该等待！)
                                   old_val = counter (5)
counter = 5 + 1 = 6
unlock() release
                                   counter = 5 + 1 = 6
                                   unlock() release
```

最终 counter = 6，而不是 7。这就是**数据竞争**。

**AsyncMutex 的保证**：
- 通过 FIFO 等待队列
- 确保一次只有一个协程在临界区
- 所以不应该发生上述竞争

## 5. 最终评价

### ✅ 正确的地方

1. **原子操作**：使用 CAS 实现无锁获取
2. **FIFO 顺序**：使用队列维护公平性
3. **Double-Check**：防止 Lost Wakeup
4. **协程友好**：暂停而不阻塞线程

### ⚠️ 可能的问题

1. **onSuspend 中的 pop 逻辑**
   - 假设队列是 LIFO 还是 FIFO？
   - 如果多个协程同时入队会怎样？
   - **答**：std::queue 是 FIFO，所以不会有问题

2. **Double-Check 时序**
   - 是否真的能防止 Lost Wakeup？
   - **答**：能，因为如果在 Double-Check 中 tryLock 成功，就不暂停
   - 如果失败，已经入队，会被后续的 unlock/wakeupNext 处理

3. **多线程调用 unlock()**
   - 虽然 AsyncMutex 是为协程设计的，但如果多个线程同时调用 unlock() 呢？
   - **答**：这会违反"未持有锁的线程不应调用 unlock()"的假设
   - 这是使用者的责任

4. **内存顺序**
   - acq_rel 是否足够？
   - **答**：对于协程（单线程）来说足够
   - 对于多线程来说，如果有多个 unlock 竞争，可能不够

## 6. 能否用数据量测出问题？

### 理论上的问题很难通过大数据量测出

原因：
- **数据竞争**是概率性的，需要特定的时序窗口
- 6000 次 lock/unlock 仍可能无法触发竞争窗口
- 需要的是**特定的时序安排**，不仅仅是数量

### 真正能暴露问题的测试应该是

1. **时序控制测试**：
   - 在特定点暂停，验证状态
   - 使用 Barrier 同步多个协程

2. **互斥性测试**：
   - 验证同一时刻只有一个协程在临界区
   - 使用原子变量标记"当前在临界区"

3. **顺序性测试**：
   - 验证 FIFO 顺序
   - 用数组记录获取锁的顺序

4. **极端情况**：
   - 协程快速退出（锁泄漏）
   - 协程被销毁（Waker 失效）
   - 多次 unlock 调用

## 7. 结论

### 理论合理性：✅ 基本正确

- 使用原子操作确保无竞争
- 使用 Double-Check 防止通知丢失
- 使用 FIFO 队列保证公平性
- 内存顺序适合协程场景

### 安全性：⚠️ 需要验证

- 关键是 Waker::wakeUp() 的实现
- 需要确保：
  1. 幂等性（多次调用没问题）
  2. 正确性（即使协程还没暂停也能处理）
  3. 内存安全（Weak_ptr 有效性）

### 测试覆盖：❓ 数据量可能不够

- 6000 次操作可能无法暴露所有竞争
- 更重要的是**测试场景的设计**
- 应该设计针对特定竞争窗口的测试

