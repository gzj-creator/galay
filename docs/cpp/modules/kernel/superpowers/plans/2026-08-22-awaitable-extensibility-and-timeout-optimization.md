# Awaitable 扩展面通用化与 Timeout 开销优化计划

> 按优先级自上而下执行；每完成一项勾选对应 checkbox。
> 验证基线：`cmake --build build/developer-full -j` 全量构建通过，
> `ctest --test-dir build/developer-full -L kernel`（至少 timeout/awaitable 相关用例）通过。

**背景**：awaitable 三层结构（`AwaitableBase` / `IOContextBase` / 具体 awaitable）与
`StateMachineAwaitable` 驱动本身是静态分发、零擦除、零分配的；问题集中在两点：
(1) 自定义 awaitable 的扩展契约靠公共成员名鸭子探测，静默错误 + facade 样板泛滥；
(2) `.timeout(ms)` 路径存在 2 次堆分配、~8 次原子 RMW、cancelled timer 滞留时间轮等不必要开销。

**设计收敛（2026-08-22）**：参考 Asio 的“操作由 awaiter 描述、完成策略由模板选择”
思路，不继续把 `Timer` 改成侵入式节点，也不把 awaiter frame 裸指针交给时间轮。新的
自定义类型优先使用 `StateMachineAwaitable<MachineT>` 或标准三件套，再以
`TimeoutSupport<Awaitable, TimeoutPolicy>` 显式指定超时注入；策略是无状态模板参数，
由编译器内联，不引入虚调用、`std::function` 或类型擦除。默认策略只作为既有 IO 类型的
过渡适配层。时间轮保留 `shared_ptr` 生命周期边界，避免跨线程 cancel 与 manager 移动
语义互相耦合。

**涉及核心文件**：
- `src/cpp/galay-kernel/core/timeout.hpp`（TimeoutTimer / TimeoutSupport / WithTimeout）
- `src/cpp/galay-kernel/core/awaitable.h`（StateMachineAwaitable / AwaitableBuilder）
- `src/cpp/galay-kernel/common/timer.hpp`、`timer_manager.hpp`、`timer_manager_mt.hpp`
- 模块 facade：`galay-rpc/kernel/rpc_conn.h`、`rpc_stream.h`、`galay-mysql/details/awaitable.h`、
  `galay-postgres/details/awaitable.h`、`galay-redis/details/pool_awaitable.h` 等

**回归风险锚点**：`test/cpp/kernel/t129_timeout_arbitration.cc` 直接访问
`WithTimeout::m_inner / m_timer`，重构必须保留这两个成员名与语义。
现有超时测试：t79（state-machine timeout）、t86（timeouterr）、t129（竞争裁决）、t120（connbld）。

---

## Phase A：自定义 awaitable 扩展面（低风险，通用性优先）

### Task A1（P0）：`ForwardingAwaitable<Derived, Inner>` CRTP mixin 消除 facade 样板

**问题**：rpc/mysql/postgres/redis/http2 共 40+ 处 facade 都是同一份
"转发 await_ready/await_suspend/await_resume + markTimeout" 的复制粘贴
（如 `rpc_conn.h:736-743`，一个文件 7 处）。

**方案**：在 `galay-kernel/core/awaitable.h` 新增：

```cpp
template <typename Derived, typename InnerT>
class ForwardingAwaitable {
public:
    // 内联转发四个 await 方法；markTimeout 仅在 Inner 支持时提供
    bool await_ready() { return m_inner.await_ready(); }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return m_inner.await_suspend(handle);
    }
    auto await_resume() { return m_inner.await_resume(); }
    void markTimeout() { m_inner.markTimeout(); }  // if constexpr 探测
protected:
    explicit ForwardingAwaitable(InnerT inner) : m_inner(std::move(inner)) {}
    InnerT m_inner;
};
```

**步骤**：
- [x] A1.1 在 `core/awaitable.h` 定义两参数 owning 形式与既有 facade 的 CRTP forwarding 形式；转发方法保持模板静态分发
- [x] A1.2 新增 `test/cpp/kernel/t148_custom_awaitable.cc`，覆盖显式 timeout policy、ready 快路径与 owning facade
- [x] A1.3-A1.4 收口 RPC、MySQL、PostgreSQL 的同构 facade；Redis Rediss 初始化 facade 同步收口
- [ ] A1.5 HTTP/2 升级 awaitable 保留自己的生命周期清理逻辑，不强行套用 forwarding mixin
- [x] A1.6 全量构建 + kernel/rpc/mysql/postgres/redis 相关 surface/timeout 测试通过

**验收**：上述 facade 文件中不再存在手写转发样板；行为不变（既有模块测试不改动而通过）。

### Task A2（P0）：超时注入定制点显式化，静默错误变编译错误

**问题**：`WithTimeout::await_resume`（timeout.hpp:525-532）对既无 `markTimeout()` 又无公共
`m_result` 成员的 inner 静默不注入超时；`awaitableStillOwnsIORegistration`（timeout.hpp:139-157）
探测 `m_controller/m_registered` 成员名。用户自定义类型命中这些路径时得到未定义结果，编译期无诊断。

**方案**：
1. 定义 `concepts::TimeoutInjectable`：满足以下任一即认为可注入超时——
   - 提供 `markTimeout()`
   - 提供 `m_result` 公共成员（历史兼容，保留）
   - 结果类型是 `std::expected<T, IOError-like>`（可通过 `setUnexpected` 注入）
2. `WithTimeout` 对不满足任何注入通道的类型 `static_assert`（带说明信息）。
3. `awaitableStillOwnsIORegistration` 增加可选定制点：inner 若提供
   `ownsIoRegistration()` 则优先调用，否则回退现有成员名探测（不破坏现有类型）。

**步骤**：
- [x] A2.1 在 `common/concepts.h` 增加 `TimeoutInjectable` / 显式 marker concepts
- [x] A2.2 `WithTimeout` 支持显式 `TimeoutPolicy`，默认适配层在无注入通道时给出编译期诊断
- [x] A2.3 `awaitableStillOwnsIORegistration` 增加 `ownsIoRegistration()` 优先探测
- [x] A2.4 新增测试 `test/cpp/kernel/t149_timeout_policy_surface.cc`：
  (a) 无注入通道的类型 + `.timeout()` → 编译失败（用 `// clang-format off` 注释保留负例或单独编译目标）；
  (b) 自定义 `ownsIoRegistration()` 的类型超时后正确返回已有结果
- [x] A2.5 全量构建 + 相关测试通过

**验收**：错误用法编译期报错；现有全部 awaitable 行为不变。

### Task A3（P1）：修复扩展面一致性裂缝

- [x] A3.1 `MysqlConnectAwaitable`（`galay-mysql/details/awaitable.h:16`）补上
  `TimeoutSupport` 继承与 `markTimeout` 转发（对齐同文件其余 3 个 awaitable）
- [x] A3.2 `PoolInitializeAwaitable::markTimeout() {}`（`pool_awaitable.h:30`）：该类型
  `await_ready() == true` 恒不挂起，确认 `.timeout()` 无意义后：移除空 `markTimeout` 并让其
  `await_ready` 恒真语义在 A2 的 static_assert 下仍合法（ready 路径不触发注入），否则补注释说明
- [ ] A3.3 mysql 模块测试补一条 connect `.timeout()` 超时用例（待真实连接 fixture）

### Task A4（P2）：文档收口

- [x] A4.1 `docs/cpp/modules/kernel/02-API参考.md`「低层组合 Awaitable 扩展面」补充
  `ForwardingAwaitable` 用法与超时注入契约（markTimeout / ownsIoRegistration / static_assert 规则）
- [x] A4.2 `docs/cpp/modules/kernel/06-高级主题.md` 第 6 节同步补充

---

## Phase B：Timeout 开销优化（按风险递增）

### Task B1（P0）：惰性分配 TimeoutTimer，同步完成路径零分配

**问题**：`WithTimeout` 构造即 `make_shared<TimeoutTimer>`（timeout.hpp:448-452）。
`await_ready() == true`（如 `ReadyAwaitable`、数据已就绪）或 `await_suspend` 同步返回 false
的路径根本不进时间轮，却照付一次 malloc + 析构 + 引用计数原子。

**方案**：把 timer 创建从构造函数移到 `await_suspend` 确认真正挂起之前。
`bindTimeoutTimer` 探测路径要求 inner 挂起前发布 timer（timeout.hpp:487-494 顺序保持不变：
bind 先于 inner.await_suspend）。嵌套 `timeout()` 重定时构造时不再传递旧 timer，
构造 `WithTimeout` 时 timer 为空，挂起时统一创建。

**步骤**：
- [x] B1.1 `WithTimeout`：成员改 `TimeoutTimer::ptr m_timer;`（空），构造只存 duration；
  `await_suspend` 在 bind 探测前 `m_timer = std::make_shared<TimeoutTimer>(m_duration)`
- [x] B1.2 `await_ready`/同步完成路径确保不解引用空 timer；`t129` 直接访问
  `m_timer` 的用例需要适配（构造后手动注入或提供测试钩子，保持其余成员名不变）
- [x] B1.3 `t148` 行为测试 + `b33_timeout_ready_path` 吞吐对照验证 ready 路径不创建 timer
- [x] B1.4 全量构建 + t79/t86/t120/t129/t148/t149 通过

**验收**：`co_await ReadyAwaitable(x).timeout(5s)` 无任何堆分配；挂起路径行为不变。

### Task B2（P1）：TimeoutTimer 状态折叠，削减冗余原子操作

**问题**：
- `timeouted()` 读 `Timer::m_flag` 的 kTimeout 位，而裁决真相在 `Completion` 状态机——
  两个状态源需要 `completeTimeout()` 内 fetch_or 同步（timeout.hpp:401-402），seq_cst。
- `await_resume` 三条路径对已 settle 的 timer 重复调用 `cancel()`（timeout.hpp:513/522/534），
  每次 `Timer::cancel` 都是一次 fetch_or + 可能的 CAS。

**方案**：
1. `TimeoutTimer::timeouted()` 改为 `m_completion.load() == kTimeoutWon`（acquire）；
   `completeTimeout` 不再写 kTimeout 位。
2. `WithTimeout::await_resume`：`timedOut` 分支内的 `m_timer->cancel()` 调用合并——
   cancel 幂等但不必重复付原子代价；保留语义：确保底层 wheel 不再触发。
3. 逐处核对 `TimerFlag::kTimeout` 的其余使用点（若有外部依赖则保留 flag 写入）。

**步骤**：
- [x] B2.1 `timeouted()` 改读 Completion；`completeTimeout`/`abortOperation` 删除对应 fetch_or
- [x] B2.2 `WithTimeout::await_resume` 路径合并冗余 cancel
- [x] B2.3 复跑 t79/t86/t120/t129；仓库内无 `TimerFlag::kTimeout` 消费方
- [x] B2.4 全量构建通过

### Task B3（取消）：侵入式时间轮节点 + cancel 摘链

**问题**：
- 每个 `.timeout()` 挂起路径：`make_shared<TimeoutTimer>`（~72B）+ `std::list` 节点分配
  （timer_manager.hpp:153），共 2 次分配、4-6 次 refcount 原子。
- `cancel()` 只设标志不摘链：cancelled timer 在 wheel 里滞留最长到到期 tick；
  `processWheel1`（timer_manager.hpp:286）甚至不检查 cancelled 直接调 `handleTimeout()`
  （靠 TimeoutTimer 内部状态机兜底，但虚调用 + 遍历成本已付）。
- `TimingWheelTimerManager` 实际单线程使用（push/tick/cancel 均在 IO scheduler 线程，
  io_scheduler.hpp:1016），shared_ptr 防的是"cancel 后 wheel 仍持有"——同线程摘链可根治。

**结论**：取消。跨线程 producer 可能调用 `TimeoutTimer::cancel()`，而 manager 又支持
移动替换；侵入式 callback 需要额外 owner/thread 状态或延迟回收队列，复杂度和额外同步
成本会抵消 list 节点收益。当前实现只在时间轮处理槽位时跳过 cancelled timer，保持
生命周期清晰；后续若 benchmark 证明 list 节点是主瓶颈，再单独设计 owner-local pool。

**步骤**：
- [x] B3.1 记录取消结论并保留 shared_ptr 生命周期边界
- [x] B3.2 时间轮处理槽位时跳过 cancelled/done timer，避免无意义虚调用

### Task B4（P3，可选大项）：io_uring `IORING_OP_LINK_TIMEOUT`

**问题**：fd 类 IO 操作的超时目前走用户态时间轮；io_uring 支持 SQE link timeout，
竞争裁决交给内核，用户态零 timer 对象、零唤醒二次调度。

**方案**：`USE_IOURING` 分支下，`WithTimeout` 若 inner 是 fd IO awaitable 且
scheduler 是 IOUringScheduler，提交操作 SQE 时附带 LINK_TIMEOUT SQE；取消即
submit REMOVE。channel/mutex 等非 fd 路径继续走 B3 的用户态方案。

- [ ] B4.1 调研 io_uring 后端 SQE 提交路径与 link 支持矩阵（内核版本、flags；本轮未执行）
- [ ] B4.2 设计探测点：inner 暴露 `attachLinkTimeout(duration)` 的类型才走内核路径
- [ ] B4.3 实现 + t152 链接超时测试（正常完成、超时、取消三路径）
- [ ] B4.4 benchmark 对比（若有 io_uring timeout 基准）

**注**：此项改动面大，若 B1-B3 后用户态开销已达标（同步路径零分配、挂起路径
零额外分配 + 少量原子），可与维护者确认是否仍需执行。

### Task C（新增）：可运行自定义 awaitable 示例

- [x] C1 `test/cpp/kernel/t148_custom_awaitable.cc`：最小三件套、显式 policy、forwarding facade
- [x] C2 `examples/cpp/kernel/include/e12_policy.cc`：跨线程 signal + timeout policy 完整示例
- [x] C2b `examples/cpp/kernel/include/e10_await.cc`：状态机 ping/pong IO 参考
- [x] C3 `benchmark/cpp/kernel/b33_timeout_ready_path.cc`：ready 路径不创建 timer 的吞吐回归

### Task D（P0，2026-08-22 TCP/UDP 10ms 超时压测后新增）：压测暴露的两类 Timeout 路径问题

**证据**（`benchmark-results/awaitable-udp-tcp-vs-asio-timeout10ms-20260822-215822/`、
`benchmark-results/galay_tcp_100ms_probe*.txt`；双方均为 recv 路径 `.timeout(10ms)`、
超时即计数重试，3 轮 CPU0 交替）：

| 场景 | galay | Boost.Asio coro |
|---|---|---|
| TCP @10ms | 6141 pkt/s，**~13% 读触发超时**，1/3 轮级联 fail | 5302 pkt/s，0.36% 超时，全 ok |
| TCP @100ms 探针（触发≈0） | **-18~25%** vs 无超时基线（5700→4300~4670） | — |
| UDP @10ms | 无超时风暴，-2% vs 基线 | 拥塞崩溃（settled 丢包 84%，全 fail） |

结论：asio 的 per-read parallel_group+steady_timer 成本约 -8%；galay 当前惰性定时器
挂起路径成本更高（@100ms ≈ -20%），且饱和负载下存在 >10ms 的事件分派停顿。
UDP 侧 asio 崩溃属于其对端延迟尾部问题，非本次优化对象。

### Task D1（P0）：削减挂起路径 per-op 定时器注册成本

**现状**：B1 完成后 ready/同步路径已零分配，但每次真正挂起的 `.timeout()` 仍付：
`make_shared<TimeoutTimer>`（控制块 + 引用计数原子）、时间轮 `std::list` 节点分配、
`addTimer` + `Timer::cancel` 的状态机原子与轮槽操作。

**方案（先测量后动手，按序执行）**：
1. D1.1 用 b31 @100ms 形态做分解测量：分别统计 alloc、wheel 插入/摘除、waker 链路耗时占比
   （perf record 或临时直方图），确认主瓶颈后再选手段；
2. D1.2 若 alloc 占比高：TimeoutTimer 改 scheduler-thread-local 对象池（free-list 复用，
   shared_ptr + 自定义 deleter 归还池；保持跨线程 cancel 语义不变，B3 取消结论仍有效——
   不做侵入式摘链，只复用节点内存）；
3. D1.3 若 wheel 操作占比高：评估 `std::list` 换 slab/vector 槽位索引存储，或
   timer 句柄化（wheel 存 `unique_ptr`，cancel 凭句柄 O(1) 置标志，处理槽位时跳过）；
4. D1.4 验收：b31 @100ms 形态相对无超时基线损耗 ≤8%（对齐 asio 水平），
   t79/t86/t120/t129/t148/t149 全过。

### Task D2（P0）：定位并消除饱和负载下 >10ms 事件分派停顿

**现象**：b31 @10ms 下 ~6700/50000 读超时（13%）；同负载同核 asio 仅 0.36%。
超时即重发策略下该停顿还会自我放大（补发→更深积压→更多超时，run1 级联 fail 的根因）。

**排查步骤**：
- [ ] D2.1 复用 `b18_ready_entry_wakeup_latency` 的测量骨架，在 b31 负载内插桩
      IO-ready→协程 resume 的延迟直方图（p50/p99/p999/max），复现 10ms 尾部；
- [ ] D2.2 二分定位来源，候选：(a) 时间轮到期批处理与 socket 事件互相阻塞；
      (b) epoll_wait 超时计算未随新挂起 timer 收紧导致唤醒滞后；(c) 单批 resume 的
      协程数量无上限造成的队头阻塞；(d) `TimeoutTimer::handleTimeout` 经由独立 waker
      二次调度引入的额外跳转；
- [ ] D2.3 对照 asio 同场景延迟直方图，量化差距收敛目标：@10ms 超时触发率 ≤1%
      （对齐 asio 0.36% 量级）；
- [ ] D2.4 修复后复跑 10ms 三轮对标：全 ok、无帧错位（settled 计数严格相等）、
      吞吐不低于无超时基线。

**注**：D2 未定位前，`.timeout()` 在饱和 TCP 负载下的压测结论（含吞吐反超市基线的假象）
不得作为正式证据归档；重试型超时语义会改变有效并发，正式对比须双侧同策略并记录触发率。

---

## 完成标准汇总

| Task | 完成标准 |
|---|---|
| A1 | 40+ 处 facade 样板由 `ForwardingAwaitable` 收口，新测试 + 模块测试通过 |
| A2 | 不可注入超时的类型编译期报错；`ownsIoRegistration()` 定制点可用 |
| A3 | MysqlConnectAwaitable 支持 `.timeout()`；空 markTimeout 裂缝消除 |
| A4 | 两份文档同步更新 |
| B1 | ready/同步完成路径零堆分配；挂起路径回归通过 |
| B2 | `timeouted()` 单一状态源；冗余原子调用消除 |
| B3 | 已取消侵入式摘链；保留 shared_ptr 生命周期边界并跳过 cancelled timer |
| B4 | （可选）io_uring fd 超时走内核 LINK_TIMEOUT |
| D1 | b31 @100ms 形态相对无超时基线损耗 ≤8%；既有 timeout 测试全过 |
| D2 | b31 @10ms 超时触发率 ≤1%，三轮对标全 ok 且 settled 计数严格相等 |
| C | 自定义 awaitable demo、显式 timeout policy 与 ready-path benchmark |

## 执行记录

- 2026-08-22：计划创建。
- 2026-08-22：收敛为模板 timeout policy；撤销侵入式时间轮节点方案，补充自定义 awaitable demo 与 ready-path benchmark。
- 2026-08-22：新增 Task D。10ms recv 超时 TCP/UDP 公平对标（galay vs asio 协程栈）暴露两类问题：
  挂起路径 per-op 定时器成本（@100ms 探针 -18~25%）与饱和负载 >10ms 分派停顿（13% 触发率）；
  明确重试型超时压测结论在 D2 收敛前不得归档为正式证据。
