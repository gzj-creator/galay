# Release Note

按时间顺序追加版本记录，避免覆盖历史发布说明。

## v3.4.5 - 2026-04-22

- 版本级别：小版本（patch）
- Git 提交消息：`chore: 发布 v3.4.5`
- Git Tag：`v3.4.5`
- 自述摘要：
  - 修复 `kqueue` reactor 的 registration token 生命周期与事件校验链路，避免 fd 复用后晚到事件命中失效 controller。
  - 修复 owner 唤醒任务被 sibling scheduler 窃取导致的跨线程恢复问题，保证 `SSL` / `Waker` 路径在所属 `IOScheduler` 线程恢复。
  - 扩展 connect 并发回归测试与 `B3-tcp_client` 连接时延统计，并清理过期的计划文档和脚本测试资产。

## v3.4.6 - 2026-04-26

- 版本级别：小版本（patch）
- Git 提交消息：`chore: 发布 v3.4.6`
- Git Tag：`v3.4.6`
- 自述摘要：
  - 修复 `io_uring` sequence socket 的 `READV` 进度推进链路，改为以 `POLLIN` 配合非阻塞读取驱动 staged sequence，避免已就绪数据被漏消费，并确保立即完成路径能够及时唤醒 owner。
  - 修复 `IOController` move 后状态转移与 ready recv 聚合消费细节，补强 `ENOBUFS`、瞬时错误与多段接收结果处理，避免接收结果丢失或错误提前上浮。
  - 修正多组调度器与通道测试的统计常量输出，并补充 `AGENTS.md` 仓库约束文档以统一目录、构建、测试与版本对齐规范。

## v4.0.0 - 2026-04-29

- 版本级别：大版本（major）
- Git 提交消息：`refactor: 统一源码文件命名规范`
- Git Tag：`v4.0.0`
- 自述摘要：
  - 将源码、头文件、测试、示例与 benchmark 文件统一重命名为 lower_snake_case，编号前缀同步改为小写下划线形式。
  - 同步更新 CMake/Bazel 构建描述、模块入口、README/docs、脚本和所有项目内 include 路径引用。
  - 移除项目内相对 include，统一使用基于公开 include 根或模块根的非相对路径。

## v4.0.1 - 2026-05-18

- 版本级别：小版本（patch）
- Git 提交消息：`fix: 修复 Task 结果存储告警并统一导出命名`
- Git Tag：`v4.0.1`
- 自述摘要：
  - 修复 `TaskResultStorageTraits` 在内联结果类型上仍实例化堆释放路径的问题，避免 `Task<std::string>` 在 GCC 优化内联后触发 `-Wfree-nonheap-object`。
  - 新增 `t121_taskresult_storage` 回归测试，在 GNU 编译器下把该告警提升为错误，并覆盖 `Task<std::string>` 的返回与消费路径。
  - 将安装导出的 CMake targets 文件改为 `galayKernelConfigTargets.cmake`，Release 安装生成 `galayKernelConfigTargets-release.cmake`，并将 CMake/Bazel 版本元数据提升到 `4.0.1`。

## v4.0.2 - 2026-05-20

- 版本级别：中版本（minor）
- Git 提交消息：`feat: 新增 BaseLogger 日志抽象接口并为所有源文件添加中文 Doxygen 注释`
- Git Tag：`v4.0.2`
- 自述摘要：
  - 新增 `BaseLogger` 虚基类、`LoggerRegistry` 全局注册中心和 `GALAY_LOG_*` 宏族，提供零开销可插拔日志基础设施。未设置 logger 时仅执行 atomic load + null check，不进入格式化。
  - 支持 `LogLevel` 五级过滤（kTrace/kDebug/kInfo/kWarn/kError），低级别消息在 `std::format` 前被截断。
  - 为全部 68 个源文件（common/kernel/async/concurrency）添加完整中文 Doxygen 注释，覆盖文件级、类级和方法级文档。
  - 导出 `logger.h` 到 C++23 module `galay.kernel`。

## v5.0.0 - 2026-05-20

- 版本级别：大版本（major）
- Git 提交消息：`refactor: 按库隔离 BaseLogger 日志入口`
- Git Tag：`v5.0.0`
- 自述摘要：
  - 移除全局 `LoggerRegistry` 和旧 `GALAY_LOG_*` 公共入口，改为 `LoggerSlot<Tag>` 按库隔离日志槽位。
  - `galay-kernel` 自身日志使用 `galay::kernel::log::set()` / `galay::kernel::log::get()` 与 `GALAY_KERNEL_LOG_*` 宏，下游库通过 `GALAY_LOG_WITH_LOGGER` 定义各自命名空间日志宏。
  - 新增 `GALAY_LOG_ENABLED` / `GALAY_KERNEL_LOG_ENABLED`，允许调用点在昂贵日志参数构造前先判断 logger 是否存在且级别是否会写入。
  - 新增 `t122_logger_slot` 回归测试，覆盖槽位隔离、kernel 日志入口，以及 logger 为空或级别过滤时不求值格式化参数的行为。

## v5.1.0 - 2026-06-07

- 版本级别：中版本（minor）
- Git 提交消息：`refactor: 发布 v5.1.0 并复用 utils 缓冲工具`
- Git Tag：`v5.1.0`
- 自述摘要：
  - 移除 `galay-kernel` 本地 `common/bytes.h` / `bytes.cc`，统一复用 `galay-utils/cache/bytes.hpp` 中的 `Bytes`、`ByteMetaData` 与字节内存辅助函数。
  - 移除 `galay-kernel/common/queue_view.h` 本地实现，协议解析示例与测试改为直接包含 `galay-utils/cache/byte_queue_view.hpp`。
  - 移除 `galay-kernel` 本地 `RingBuffer` 实现，`galay-kernel/common/buffer.h` 通过 using 保留 `galay::kernel::RingBuffer` 入口并复用 `galay-utils/cache/ring_buffer.hpp`。
  - `galay-kernel` CMake 与 package config 声明依赖 `galay-utils >= 3.1.0`，优先使用已安装的 `galay::galay-utils` 目标，开发构建仍支持 `GALAY_UTILS_INCLUDE_DIR`。

## v5.2.0 - 2026-06-11

- 版本级别：中版本（minor）
- Git 提交消息：`chore: 发布 v5.2.0 并提升 galay-utils 依赖`
- Git Tag：`v5.2.0`
- 自述摘要：
  - 将 `galay-utils` 最低依赖版本从 `3.1.0` 提升到 `3.2.0`，同步更新 CMake package config 模板中的版本约束与错误提示。
  - 将 CMake 与 Bazel 版本元数据提升到 `5.2.0`，与本次 minor tag 对齐。

## v5.1.1 - 2026-06-09

- 版本级别：小版本（patch）
- Git 提交消息：`chore: 发布 v5.1.1 并同步版本元数据`
- Git Tag：`v5.1.1`
- 自述摘要：
  - 将 `Runtime::blockOn()`、`Runtime::spawn()`、`Runtime::spawnBlocking()` 与 `RuntimeHandle` 提交接口改为通过 `std::expected` 返回错误，runtime API 边界不再使用 `throw` / `try` / `catch` 传播失败。
  - 新增 `RuntimeError`、`TaskResultError` 与 `BlockingExecutorError`，通过无分配的 `message()` 将错误码映射为可读错误原因。
  - `JoinHandle::wait()` / `join()` 与 `co_await Task<T>` 结果消费路径改为返回 `std::expected`，重复消费、结果缺失、调度失败等错误继续通过返回值传播。
  - 新增 runtime expected 源码边界测试，并更新任务、spawn、join、await、blocking executor 相关测试与 include/import 示例。
  - 将 CMake 与 Bazel 版本元数据提升到 `5.1.1`，与本次 patch tag 对齐。
