# MPSC 通道性能分析报告

**日期:** 2026-08-04
**版本:** v1.0
**状态:** 待优化

## 执行摘要

通过对比 galay MPSC 通道与 Rust crossbeam-channel 的实现，发现性能差距主要来自：

1. **过度的内存屏障开销** - 热路径使用过多 `seq_cst`
2. **复杂的 waiter 协调机制** - pump 系统引入额外竞争
3. **生产者注册开销** - 每次发送都要协调关闭状态
4. **TLS 缓存查找效率低** - 线性搜索多个 channel

**预期性能差距:**
- 有界通道单生产者: 30-50% 慢
- 有界通道多生产者: 50-100% 慢
- 无界通道单生产者: 40-60% 慢
- 无界通道多生产者: 100-200% 慢

## 1. 有界通道 (BoundedChannel) 问题

### 1.1 内存序过于保守

**问题代码 (bounded_channel.h:962):**
```cpp
// 发送路径 - 每条消息都执行 SEQ_CST
slot->sequence.store(position + 1, std::memory_order_seq_cst);
```

**对比 crossbeam:**
- 使用 `Release` 发布消息
- 只在必要的跨线程同步点使用 `SeqCst`

**性能影响:**
- x86 平台 SEQ_CST 比 Release 慢 2-3x
- 每条消息至少 2 次 SEQ_CST (发布 + 检查等待者)
- 高吞吐场景下累积开销显著

### 1.2 等待者计数检查开销

**问题代码 (bounded_channel.h:505):**
```cpp
if (m_recvWaiterCount.load(std::memory_order_seq_cst) != 0) {
    requestPump(kRecvWork);
}
```

**问题:**
- 即使没有等待者，每次发送都要检查
- SEQ_CST 读取强制全局同步
- 成为多生产者竞争热点

### 1.3 Pump 系统复杂度

**问题代码 (bounded_channel.h:1351-1375):**
```cpp
void runPump() noexcept {
    WaiterPtr wakeHead;
    WaiterPtr wakeTail;
    // 复杂的 waiter 队列处理
    // 每个 waiter:
    // - Timeout timer 检查
    // - 多次 CAS 状态转换
    // - 重新入队逻辑
    // - Deferred wake 链表管理
}
```

**对比 crossbeam:**
- 等待者直接在自己线程重试
- 没有集中式 pump
- 更简单的唤醒机制

**性能影响:**
- 额外的竞争点
- 多次原子操作增加延迟
- Waiter 队列管理开销
