# AsyncMutex 测试说明

## 快速编译运行

```bash
cd build
cmake ..
make test_async_mutex
./bin/test_async_mutex
```

## 测试数据量

### 测试1: 单个 Handle 内多个协程竞争
- **协程数**: 20 个
- **每个操作数**: 100 次
- **总操作数**: 2,000
- **验证**: 数据竞争检测

### 测试2: 极限高竞争场景
- **协程数**: 50 个
- **每个操作数**: 50 次
- **总操作数**: 2,500
- **验证**: 无死锁，计数器精确性

### 测试3: 互斥性验证
- **协程数**: 30 个
- **每个操作数**: 50 次（每次检查 isLocked()）
- **总操作数**: 1,500
- **验证**: 持有锁时 isLocked() 的准确性

## 总计

**6,000 次 lock/unlock 操作** - 足以暴露任何竞争条件或死锁问题

## 关键验证点

1. **数据竞争**: 计数器的最终值必须完全准确
2. **死锁检测**: 所有协程必须在超时前完成
3. **语义正确性**: isLocked() 在临界区内必须为 true
4. **错误计数**: errors 计数器必须为 0

## 预期输出

```
╔══════════════════════════════════════════════════════╗
║    AsyncMutex Test - High Volume & Race Detection   ║
║   Total Operations: 2000 + 2500 + 1500 = 6000      ║
╚══════════════════════════════════════════════════════╝

====== Test 1: Single Handle - Multiple Coroutines (High Volume) ======
Spawning 20 workers, 100 iterations each...
Expected: 2000, Actual: 2000 ✓ PASS

====== Test 2: Extreme Contention (50 workers) ======
Spawning 50 workers, 50 iterations each (total: 2500 ops)...
Expected: 2500, Actual: 2500 ✓ PASS

====== Test 3: Mutex Semantics (30 workers × 50 ops) ======
Spawning 30 workers, 50 iterations each (total: 1500 ops)...
Expected: 1500, Actual: 1500 ✓ PASS

╔══════════════════════════════════════════════════════╗
║                All tests completed!                  ║
╚══════════════════════════════════════════════════════╝
```

## 如果测试失败

- **计数器不匹配**: 说明有严重的数据竞争或死锁
- **程序超时/挂起**: 说明有死锁（20 秒后仍未完成）
- **ERROR 消息**: isLocked() 返回值有问题
- **errors 计数非零**: 语义错误

## 执行时间

每个测试给予充足的等待时间：
- Test 1: 5 秒
- Test 2: 8 秒
- Test 3: 7 秒
- 总计: 约 20 秒

这个数据量足以检测出任何 AsyncMutex 的线程安全问题！

