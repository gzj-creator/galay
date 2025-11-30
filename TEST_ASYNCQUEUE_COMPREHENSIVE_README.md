# AsyncQueue 全面测试套件说明

## 文件位置
```
test/test_async_queue_comprehensive.cc
```

## 快速编译运行

```bash
cd build
cmake ..
make test_async_queue_comprehensive
./bin/test_async_queue_comprehensive
```

## 测试覆盖范围

### 测试1: 单 Handle 多生产者单消费者
**文件中的函数**: `test_single_handle_multi_coro()`

**场景**：
- 5 个生产者协程，每个生产 20 条数据（共 100）
- 1 个消费者协程，消费全部数据
- 所有运行在同一个 Scheduler（Handle）

**验证内容**：
- ✓ 所有数据都被生产
- ✓ 所有数据都被消费
- ✓ 没有错误或异常

**预期结果**：
```
Produced: 100, Consumed: 100, Errors: 0 ✓ PASS
```

---

### 测试2: 多 Handle 生产，单 Handle 消费
**文件中的函数**: `test_multi_handle_producers()`

**场景**：
- 3 个不同 Scheduler（Handle）
- 每个 Handle 上有 2 个生产者，每个生产 15 条数据
- 总共：3 × 2 × 15 = 90 条数据
- 单独的 Scheduler 上有 1 个消费者

**验证内容**：
- ✓ 跨 Handle 的生产者能正确生产数据到共享队列
- ✓ 单 Handle 消费者能从多个生产者消费
- ✓ 没有死锁（多 Handle 并发访问队列）

**关键测试点**：
```
多线程场景：
  Handle 0: Producer 0, 1 → enqueue()
  Handle 1: Producer 2, 3 → enqueue()
  Handle 2: Producer 4, 5 → enqueue()
  Handle 3: Consumer → waitDequeue()
```

**预期结果**：
```
Produced: 90, Consumed: 90, Errors: 0 ✓ PASS
```

---

### 测试3: 单 Handle 多生产者多消费者
**文件中的函数**: `test_multi_producer_consumer()`

**场景**：
- 4 个生产者，每个生产 25 条数据（共 100）
- 4 个消费者（3 个消费约 25 条，1 个消费约 33 条）
- 所有协程运行在同一个 Scheduler

**验证内容**：
- ✓ 多个消费者不会相互竞争导致错误
- ✓ 每条数据只被消费一次（不会重复或丢失）
- ✓ 所有消费者都能正常工作

**竞争场景**：
```
生产者竞争入队    消费者竞争出队
   ↓                  ↓
Producer 0  ┐      Consumer 0 ┐
Producer 1  ├→ Queue ←┤ Consumer 1
Producer 2  ├→ Queue ←┤ Consumer 2
Producer 3  ┘      Consumer 3 ┘
```

**预期结果**：
```
Produced: 100, Consumed: 100, Errors: 0 ✓ PASS
```

---

### 测试4: FIFO 顺序验证
**文件中的函数**: `test_fifo_order()`

**场景**：
- 单个生产者生产 500 个连续数字 (0-499)
- 单个消费者按顺序消费验证

**验证内容**：
- ✓ 消费的顺序严格为 0, 1, 2, ..., 499
- ✓ 没有跳过或重复任何数据
- ✓ AsyncQueue 维护 FIFO 语义

**验证代码**：
```cpp
if (value != last_value + 1) {
    // 顺序错误！
}
```

**预期结果**：
```
All values in correct FIFO order ✓ PASS
Produced: 500, Consumed: 500, Errors: 0 ✓ PASS
```

---

## 测试统计数据

| 测试 | 总数据量 | 生产者数 | 消费者数 | Handle数 |
|------|---------|---------|---------|---------|
| 测试1 | 100 | 5 | 1 | 1 |
| 测试2 | 90 | 6 | 1 | 4 |
| 测试3 | 100 | 4 | 4 | 1 |
| 测试4 | 500 | 1 | 1 | 1 |
| **总计** | **790** | **16** | **7** | **4** |

---

## 关键测试场景

### 场景A: Lost Wakeup 检测
```
如果 AsyncQueue 有 Lost Wakeup bug：
- 生产者 push 并 notify
- 消费者 waitDequeue 中间没能被唤醒
- 导致消费不完全
→ consumed != produced，测试失败
```

### 场景B: 数据竞争检测
```
如果没有正确同步：
- 多个消费者同时 dequeue
- 可能同一条数据被消费两次
- 或数据被破坏
→ errors 计数增加，测试失败
```

### 场景C: 死锁检测
```
如果有死锁：
- 消费者永久等待
- 生产者无法继续
- 测试超时（20 秒后仍未完成）
→ produced < expected，测试失败
```

### 场景D: 跨 Handle 同步
```
如果多 Handle 访问不线程安全：
- 多个 Handle 的生产者同时 push
- 消费者无法正确 dequeue
→ consumed != produced，测试失败
```

---

## 执行流程

```
1. RuntimeBuilder 创建 4 个 CoScheduler（Handle）
2. Test 1 运行（~1s）
3. Test 2 运行（~2s，多 Handle）
4. Test 3 运行（~2s，多生产消费）
5. Test 4 运行（~1s，FIFO 验证）
6. 所有测试完成
7. Runtime 停止

总执行时间：~8-10 秒
```

---

## 预期输出示例

```
╔════════════════════════════════════════════════════════╗
║       AsyncQueue Comprehensive Test Suite              ║
╚════════════════════════════════════════════════════════╝

╔════════════════════════════════════════════════════════╗
║ Test 1: Single Handle - Multiple Producers & Consumer ║
╚════════════════════════════════════════════════════════╝
Spawning 5 producers and 1 consumer on same handle...
[Handle 0] Producer 0 produced: 0
[Handle 0] Producer 1 produced: 1000
[Handle 0] Consumer received: 0
...
Results:
  Expected produced: 100, Actual: 100
  Expected consumed: 100, Actual: 100
  Errors: 0
✓ Test 1 PASSED

╔════════════════════════════════════════════════════════╗
║ Test 2: Multiple Handles Producer - Single Consumer    ║
╚════════════════════════════════════════════════════════╝
Spawning 3 producer handles and 1 consumer handle...
[Handle 0] Producer 0 produced: 0
[Handle 1] Producer 0 produced: 10000
[Handle 2] Producer 0 produced: 20000
[Consumer] Received: 0
...
Results:
  Expected produced: 90, Actual: 90
  Expected consumed: 90, Actual: 90
  Errors: 0
✓ Test 2 PASSED

╔════════════════════════════════════════════════════════╗
║ Test 3: Single Handle - Multiple Producers & Consumers║
╚════════════════════════════════════════════════════════╝
Spawning 4 producers and 4 consumers...
[Consumer 0] Got: 0
[Consumer 1] Got: 1
...
Results:
  Expected produced: 100, Actual: 100
  Expected consumed: 100, Actual: 100
  Errors: 0
✓ Test 3 PASSED

╔════════════════════════════════════════════════════════╗
║ Test 4: FIFO Order Verification (500 items)           ║
╚════════════════════════════════════════════════════════╝
✓ All values in correct FIFO order
Results:
  Total produced: 500
  Total consumed: 500
  Errors: 0
✓ Test 4 PASSED

╔════════════════════════════════════════════════════════╗
║              All tests completed!                      ║
╚════════════════════════════════════════════════════════╝
```

---

## 故障排查

### 如果 Test 1 或 3 失败
- 检查 AsyncQueue 的 waitDequeue() 实现
- 查看是否有 Lost Wakeup 问题（推送和等待之间的竞争）
- 验证 AsyncWaiter 的通知机制

### 如果 Test 2 失败
- 检查 ConcurrentQueue 的线程安全性
- 验证跨 Handle 的同步是否正确
- 查看是否有多线程访问队列时的竞争

### 如果 Test 4 失败
- 检查 FIFO 顺序是否被破坏
- 验证队列是否真的是先进先出
- 查看是否有数据被重复消费或跳过

### 如果程序超时（20+ 秒未完成）
- 说明有死锁
- 检查 waitDequeue() 是否正确唤醒
- 检查生产者是否正确 notify

---

## 注意事项

1. **多 Handle 场景**：
   - RuntimeBuilder 的 `setCoSchedulerNum()` 需要支持
   - 确保 Runtime 能创建多个 Scheduler

2. **数据竞争检测**：
   - 使用 `produced` 和 `consumed` 的原子计数
   - 任何不匹配都表示有问题

3. **FIFO 验证**：
   - 最严格的测试
   - 如果通过，说明基本正确
   - 如果失败，说明有严重的顺序问题

4. **性能考虑**：
   - 每个消费操作都有 10ms-5ms 的睡眠（模拟处理）
   - 这会影响整体执行时间
   - 生产速度通常比消费快

