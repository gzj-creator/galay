# RingBuffer 模板化重构任务文档

## 任务概述

将 `RingBuffer` 重构为模板类，支持编译期选择后端策略（`Mmap`/`Vector`/`Auto`），默认策略为 `Mmap`（单段连续视图，消除环绕边界的临时拷贝）。

## 背景

现有 `RingBuffer` 存在以下问题：

1. **2 段环绕 API 导致强制拷贝**：Redis/MySQL 解析器必须把两段 iovec 拷贝到临时 `parse_buffer` 才能解析，零拷贝 API 被 defeat。`iovec_utils.h` 有 500 行代码处理这个问题。
2. **`writeSpans`/`readSpans` 基本不用**：生产代码只用 iovec(~77 次调用) vs span(~9 次全部在测试)。
3. **`MmapRingBufferImpl`**(≥64KB)已证明单段是正确方向：它用双映射 mmap 让环绕边界也呈现为单段连续。

## 目标

1. `RingBuffer` 模板化，支持 `RingBufferBackendStrategy` 模板参数（默认 `Mmap`）。
2. Protocol client（MySQL/Redis/RPC 等）带模板参数，默认 `Mmap`。
3. `Mmap` 策略返回单段 `std::span<std::byte>`/单个 `iovec`。
4. `Vector` 策略保持 2 段环绕行为（向后兼容）。

## 核心变更

### 1. 新增 `RingBufferBackendStrategy` 枚举

在 `src/cpp/galay-utils/cache/ring_buffer.hpp` 中：

```cpp
namespace galay::utils {

enum class RingBufferBackendStrategy {
    Mmap,   // 双映射 mmap，单段连续视图
    Vector, // vector 分配，2 段环绕视图
    Auto    // 自动选择：≥64KB 用 mmap，<64KB 用 vector
};

} // namespace galay::utils
```

### 2. `RingBuffer` 模板化

```cpp
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class RingBuffer {
    // ... 现有接口保持不变
    // 内部用 if constexpr 或 std::conditional 选择后端
};
```

### 3. Protocol client 模板化

```cpp
// src/cpp/galay-mysql/async/client.h
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class AsyncMysqlClient {
    RingBuffer<Strategy> m_ring_buffer;
    // ... 其余代码不变
};

// 使用时：
AsyncMysqlClient<> client1;                          // 默认 Mmap
AsyncMysqlClient<RingBufferBackendStrategy::Vector> client2;   // 显式 Vector
```

类似地修改：
- `src/cpp/galay-redis/async/redis_client.h`
- `src/cpp/galay-rpc/kernel/rpc_stream.h`
- `src/cpp/galay-rpc/kernel/rpc_conn.h`
- `src/cpp/galay-http2/client/h2c_client.h`
- `src/cpp/galay-ws/kernel/ws_conn.h`（如有 RingBuffer 成员）

### 4. 行为差异

| 策略 | writeSpans/readSpans | getWriteIovecs/getReadIovecs |
|------|---------------------|------------------------------|
| `Mmap` | 返回 1 个 span（单段连续） | 返回 1 个 iovec（单段连续） |
| `Vector` | 返回最多 2 个 span（环绕时） | 返回最多 2 个 iovec（环绕时） |
| `Auto` | 取决于容量（≥64KB 同 Mmap，<64KB 同 Vector） | 同左 |

## 实现步骤

### Step 1: 新增 `RingBufferBackendStrategy` 枚举

在 `ring_buffer.hpp` 的 `galay::utils` namespace 中新增枚举。

### Step 2: `RingBuffer` 模板化

1. 将 `class RingBuffer` 改为 `template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap> class RingBuffer`。
2. 内部 `Impl` 类型根据 `Strategy` 选择：
   ```cpp
   #if GALAY_UTILS_RING_BUFFER_HAS_MMAP
   using Impl = std::conditional_t<
       Strategy == RingBufferBackendStrategy::Mmap,
       detail::MmapRingBufferImpl,
       detail::VectorRingBufferImpl
   >;
   #else
   using Impl = detail::VectorRingBufferImpl;
   #endif
   ```
3. `Auto` 策略需要运行时判断容量，保留现有 `kMmapThreshold` 逻辑。

### Step 3: 修改 Protocol client

逐个修改以下文件，添加模板参数：

1. `src/cpp/galay-mysql/async/client.h`
   - `template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap> class AsyncMysqlClient`
   - `RingBuffer<Strategy> m_ring_buffer`
   - 相关 awaitable/inner classes 也需要模板化

2. `src/cpp/galay-redis/async/redis_client.h`
   - 类似 MySQL 的改动

3. `src/cpp/galay-rpc/kernel/rpc_stream.h`
   - `template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap> class RpcStream`
   - 持有的 `RingBuffer*` 改为 `RingBuffer<Strategy>*`

4. 其他 protocol client 同理

### Step 4: 更新测试

1. `test/cpp/utils/t1_ring_buffer_iovec.cc`
   - 测试默认策略（Mmap）断言 count == 1
   - 新增测试 Vector 策略断言 count 可能是 2

2. `test/cpp/utils/t6_buffer_queue_ring.cc`
   - 测试两种策略

3. `test/cpp/utils/t16_mmap_ring_buffer.cc`
   - 应完全不受影响

4. `test/cpp/kernel/t18_ringio.cc`
   - 更新断言

### Step 5: 运行测试验证

```bash
cmake --build build
ctest --test-dir build --output-on-failure -R 'utils|kernel'
```

## 关键文件

### 修改
- `src/cpp/galay-utils/cache/ring_buffer.hpp` —— 核心接口
- `src/cpp/galay-mysql/async/client.h` —— MySQL client 模板化
- `src/cpp/galay-redis/async/redis_client.h` —— Redis client 模板化
- `src/cpp/galay-rpc/kernel/rpc_stream.h` —— RPC stream 模板化
- `src/cpp/galay-rpc/kernel/rpc_conn.h` —— RPC conn 模板化
- `src/cpp/galay-http2/client/h2c_client.h` —— HTTP2 client 模板化

### 不受影响
- `src/cpp/galay-kernel/async/tcp_socket.h` —— 不持有 RingBuffer，只接受 `iovec*`

## 注意事项

1. **二进制膨胀**：每个不同的 `Strategy` 实例化会生成一份代码。可通过显式实例化缓解（`ring_buffer.cc` 中 `template class RingBuffer<RingBufferBackendStrategy::Mmap>`）。
2. **`TcpSocket` 不变**：它只接受 `iovec*` 调 `readv`/`writev`，不持有 RingBuffer，无需模板化。
3. **向后兼容**：`Vector` 策略保持 2 段环绕行为，现有代码显式指定 `Vector` 策略时可保持原有行为。
4. **默认行为变更**：默认策略从 `Auto`（阈值 64KB）改为 `Mmap`，网络场景默认单段视图。

## 完成判据

- [ ] `RingBufferBackendStrategy` 枚举已添加
- [ ] `RingBuffer` 已模板化，默认 `Mmap`
- [ ] Protocol client 已模板化（MySQL/Redis/RPC/HTTP2）
- [ ] 测试全部通过（单元 + 集成）
- [ ] 默认策略（Mmap）返回单段视图
- [ ] `Vector` 策略保持 2 段环绕行为
