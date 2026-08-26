# 并行执行模型计划评审

评审输入：`docs/plans/2026-08-24-execution-parallel-model.md`。

## 结论

原计划可以作为背景，但不能直接作为实现规格。它描述了 `bulk()`、扁平的
`when_all()`、`when_any()` 和 `parallel_for()`，没有定义用户要求的
`co_await parallel(tasks)` DAG 入口，也没有闭合“全部执行完毕后才唤醒”的
生命周期协议。

主要缺口如下：

- 没有节点 ID、依赖边、入度计算、拓扑/环检测或后继释放规则；
- `remaining` 只统计已提交任务，提交失败会使父协程永久挂起；
- 空输入的 `await_ready()` 会绕过 context，但 `await_resume()` 仍解引用空
  context；
- `await_suspend(std::coroutine_handle<>)` 无法从句柄取得 galay 的 `TaskRef`；
- `when_any()` 用短路 `||`，首个成功任务不会递减完成计数；
- `Task<T>` 已经把内部结果包装成 `std::expected`，再用
  `result.has_value()` 判断业务返回类型会混淆两层错误；
- 每个 chunk/wrapper 都创建 `Task<void>`，普通同步计算会多一个 coroutine frame；
- 全局 `ParallelSchedulerPool` 与 `Runtime` 自己持有的 scheduler 集合、停止顺序
  和多 Runtime 生命周期冲突；
- 对象池、CAS 错误记录、批量提交和纳秒级指标都没有边界测试或实测依据。

## 目标语义

第一版应把核心收敛为 `ParallelGraph` + `parallel(ParallelGraph&&)`：

```cpp
ParallelGraph graph;
auto a = graph.add([]() noexcept { load(); });
auto b = graph.add([]() noexcept { transform(); });
auto c = graph.add([]() noexcept { merge(); });
graph.then(*a, *b);
graph.then(*a, *c);
auto result = co_await parallel(std::move(graph));
```

普通 `noexcept` callable 作为同步 work item，直接进入 `ParallelScheduler` 的
工作队列，不为每个节点创建协程。需要挂起或已有 `Task<T>` 语义的节点另做
显式 adapter，不和同步 work 混用。

每个节点必须恰好一次进入 terminal（succeeded、failed、skipped 或 rejected）。
节点完成时先发布自己的结果，再处理后继，最后执行：

```text
remaining_terminal.fetch_sub(1, acq_rel)
    == 1 -> completed.store(true, release)
             requestTaskResume(parent)
```

父协程只能由其 owner scheduler 的 `scheduleResume()` 恢复，不能从工作线程直接
调用 `coroutine_handle.resume()`。`await_resume()` 用 acquire 读取图结果。提交
失败和 scheduler 不可用也必须转成 terminal，不能留下未递减的节点。

失败策略先采用“记录首错、排空已接受工作”：运行中的节点继续，失败节点的
pending 后继标记为 skipped；所有节点 terminal 后才恢复父协程。取消 token 不在
第一版实现，但不能用永久挂起代替取消语义。

## 本 worktree 的实现

当前分支 `codex/execution-parallel-model-v2`、路径
`.worktree/execution-parallel-model` 已实现第一版：

- `ParallelWorkItem`：不拥有 coroutine frame 的同步计算队列项；
- `ParallelWork` / `ParallelGraph`：DAG、重复边和环检测；
- `parallel(graph)`：Runtime scheduler 选择、节点状态机、首错、skip/reject、
  图级完成闩锁和 owner scheduler 唤醒；
- 单节点 compute-root 走当前 worker 的同步快路径；空图不挂起；
- `t181_parallel_dag`：diamond 顺序、失败排空、环检测、单节点、空图和无
  parallel scheduler 边界；
- `b35_parallel_work_item`：同步 work item 与每节点 coroutine 基线的对比基准。

这版暂时只接受 `void` 或 `std::expected<void, ParallelError>` 的 `noexcept`
同步 callable；异构结果 tuple、`when_any` 和 `Task<T>` adapter 应在该状态机
通过压力测试后再扩展。

## 后续验收

先运行 `t181_parallel_dag` 和现有 compute/runtime 测试，再运行
`benchmark_kernel_parallel_work_item`。只有基准显示真实收益后，才考虑对象池、
批量入队、缓存行填充等优化；不预先承诺固定纳秒阈值。
