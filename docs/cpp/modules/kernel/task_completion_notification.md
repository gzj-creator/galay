# Task 完成通知协议与验证

验证日期：2026-10-08。

任务完成路径移除 `TaskWaiter`、惰性 waiter 分配、waiter 指针读取及 notifier 的
mutex 获取，改用一个 32 位原子状态与 C++20 `atomic::wait/notify_all`。无阻塞
等待者时保留一次原子交换，不执行通知。当前 x86_64 构建中 `TaskState` 仍为
128 字节、64 字节对齐。

## 原协议的成本与必要性

原 `notify_task_waiters` 只有在 waiter 指针非空时才获取 mutex，不能把它描述成
每个任务完成都加锁。每次完成都会执行的是 `m_done.store(true, seq_cst)` 和
`m_waiter.load(seq_cst)`；本地 GCC 14 的完成写入编译为字节 `xchg`。

mutex 防止等待线程检查 `m_done == false` 后、真正进入 condition variable 等待
前丢失通知。`seq_cst` 则保证 waiter 发布与完成发布之间的跨原子握手：等待方
不能同时看不到完成、完成方也看不到 waiter。只删除 mutex 或把这些内存序
改成 acquire/release，无法保持原协议的正确性。

## 新协议与正确性

状态转移只有以下三种，`kDone` 是终态：

| 操作 | 状态转移 | 内存序与行为 |
| --- | --- | --- |
| 首个同步等待者注册 | `kPending -> kWaiting` | CAS；失败时 acquire 读取当前状态 |
| 没有等待者时完成 | `kPending -> kDone` | release exchange；不通知 |
| 已有等待者时完成 | `kWaiting -> kDone` | release exchange；通知全部等待者 |

CAS 和 exchange 使用同一个原子变量，具有共同的修改序。注册先发生时，完成
exchange 必须看到 `kWaiting` 并通知。完成先发生时，CAS 失败并 acquire 读取
`kDone`，等待方直接返回。其他等待者读取 `kWaiting` 后也等待同一个状态。

`atomic::wait(kWaiting, acquire)` 在值已经改变时直接返回，标准库负责解决检查
与入睡之间的竞争；`kDone` 不会再变回 `kWaiting`，没有 ABA。结果写入先于
release 完成发布，成功等待或 acquire 读取 `kDone` 后可以观察这些写入。
等待与完成调用方继续持有有效任务引用，状态不会在通知或等待期间被回收。

所有原 `m_done` 读取迁移为 `TaskState::is_done()`，仅比较 `kDone`，避免把
非零的 `kWaiting` 当作完成。公开 `Task::done()`、`JoinHandle::wait/join()` 和
父子协程 continuation 语义保持；同步等待仍阻塞调用线程。

采用标准库原子等待，无新增依赖或平台专用 futex 实现。32 位状态在本地
libstdc++ 的 Linux 实现中可直接作为 futex 等待地址；无等待者时跳过通知，也
避免 `atomic<bool>::notify_all()` 访问共享版本计数。新的状态原子有
`is_always_lock_free` 编译期检查，标准等待接口本身的实现由平台标准库决定。

## 验证范围

环境为 Linux x86_64、GCC 14.2、libstdc++、epoll、C++23 Release 共享库，构建
启用 `-Werror`。原实现与候选使用各自头文件重新编译，独立证据目录为
`/tmp/galay-task-completion-20261008/`。

- 原实现的 9 项 Task 定向回归通过；补充首个 waiter 已注册的多等待者回归后，
  原实现的 T198 再次通过。
- 候选 19 项 kernel 回归通过，包含 Task API、结果存储、awaiter 顺序、帧分配、
  生命周期、executor 分离、并行 DAG/关闭及六类 channel 超时竞争。
- Redis 拓扑任务回归 1/1 通过。
- 本地 kernel ASan/UBSan/泄漏检查 5/5 通过，包含 T198、结果存储、生命周期、
  continuation 与子任务等待。
- 本地 kernel TSan 定向检查 4/4 通过，包含 T198、生命周期、continuation 与
  子任务等待。初次运行有 3 项在测试启动前报 `unexpected memory mapping`；
  对测试进程使用 `setarch x86_64 -R` 后全部通过，无 suppressions。第三方队列
  既有 `atomic_thread_fence` TSan 支持告警保留，TSan 通过不替代协议证明。
- 14 个模块 prelude 校验通过。LLVM 22.1.8 / libc++ 的 `galay.kernel` 接口和
  对象编译通过；E4 模块消费者提供所需 `<coroutine>` 后编译通过。该消费者
  未链接或运行，不据此声明完整 mcpp 或其他平台回归通过。

T198 新覆盖完成早于等待、等待重复调用不消费结果、重复 join 的错误、八个
线程共享等待、等待不改变 `done()`、非原子写入可见性及释放任务持有者后的
结果可用性。原 50,000 轮双 IO scheduler 完成/wait/join 竞争保留，共 100,000
个任务。

## 本地开销对比

两版均为普通 Release，没有 LTO/PGO。下表是局部测量，不代表业务吞吐结论：

| 场景 | 原实现中位耗时 | 候选中位耗时 | 观察 |
| --- | --- | --- | --- |
| 没有等待者的完成函数 | 9.738 ns/次 | 9.442 ns/次 | 约降低 3.0% |
| B35 任务创建、执行、销毁 | 54.784 ns/任务 | 54.815 ns/任务 | 基本持平 |
| 已注册一个等待者时的完成函数 | 2.820 us/次 | 0.692 us/次 | 定向探针约降低 75% |

前两个场景固定 CPU 2，每场景五个 ABBA/BAAB 交替块，两版各十个进程。
完成函数探针每个进程测 64,000,000 次，复用 256 个状态，重置与分配不计入
耗时；B35 每个进程执行 10,000,000 个完整任务。没有删除不利样本。

注册等待者场景只运行一个 ABBA 块，两版各两个进程，各处理 200,000 个独立
任务状态，两个线程允许使用 CPU 2–3；完成方先观察真实注册，再计时完成函数。
该探针包含标准库通知和 mutex 竞争，不能保证每次通知前线程已在操作系统
中睡眠，也不包含任务创建、等待注册、结果消费或调度恢复的全部耗时，因此
75% 只作定向诊断，不能推广为真实 join 延迟或整体应用收益。

x86 上新旧完成操作都仍有一次 `xchg`。新协议没有消除跨线程同步的全部
成本，主要价值是使完成线程无需获取 mutex，并删除 waiter 分配与指针握手。
无等待者场景的完整 Task 生命周期未测得可区分的收益。io_uring、原生
macOS/BSD、AArch64、完整应用负载及跨 NUMA 性能未在本轮测量。

主要证据：`candidate-ctest.log`、`redis-ctest.log`、`asan-ctest.log`、
`tsan-ctest.log`、`tsan-no-aslr-ctest.log`、`performance-raw.jsonl`、
`performance-summary.json`、`registered-performance.json`、两版
`completion.asm` 及 `module/compile-*.log`。
