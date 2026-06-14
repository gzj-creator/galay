# 18-运行时Runtime

本页现在只保留 `Runtime` 专题的定位信息；完整事实请优先回到主干页。

## 本页回答什么

- `Runtime` / `RuntimeBuilder` 负责什么
- 调度器数量、轮询分发、全局定时器这些问题先看哪里
- 哪些测试 / 示例最适合作为运行时语义锚点

## 当前稳定事实

- `Runtime` 负责统一管理多个 IO / 计算调度器
- `RuntimeBuilder` 负责调度器数量与高级绑核策略等配置
- 高层任务入口现在包括 `Runtime::blockOn(Task<T>)`、`Runtime::spawn(Task<T>)`、`Runtime::spawnBlocking(...)`
- `JoinHandle<T>` 的公开结果路径收口为 `join()` / `wait()`
- runtime 上下文可通过 `Runtime::handle()`、`RuntimeHandle::current()`、`RuntimeHandle::tryCurrent()` 获取
- `Task<void>::then(...)` 是当前保留的链式根任务接口
- `TaskRef`、协程绑定与 resume plumbing 已收敛为 runtime/scheduler 内核细节，不再作为高层工作流 API 推荐
- `start()` / `stop()`、调度器轮询、全局 `TimerScheduler` 的完整说明已折回主干页

## 先看主干页

- 架构与调度模型：`docs/01-架构设计.md`
- 公开 API：`docs/02-API参考.md`
- 最小工作流：`docs/03-使用指南.md`
- 绑核 / 平台高级约束：`docs/06-高级主题.md`

## 源码 / 验证锚点

- 源码：`galay-kernel/kernel/runtime.h`、`galay-kernel/kernel/runtime.cc`
- 关联类型：`galay-kernel/kernel/task.h`、`galay-kernel/kernel/compute_scheduler.h`、`galay-kernel/kernel/io_scheduler.hpp`
- 测试：`test/t10_compute.cc`、`test/t11_mixed.cc`、`test/t22_runtime.cc`、`test/t37_rtcounts.cc`、`test/t46_blockres.cc`、`test/t48_joinhandle.cc`、`test/t49_handle.cc`、`test/t50_spawnblk.cc`
- 示例：`examples/include/e2_echo.cc`、`examples/include/e3_tcp.cc`、`examples/include/e4_task.cc`

## RAG 关键词

- `Runtime`
- `RuntimeBuilder`
- `Task`
- `JoinHandle`
- `RuntimeHandle`
- `blockOn`
- `spawnBlocking`
- `start`
- `stop`
- `getNextIOScheduler`
