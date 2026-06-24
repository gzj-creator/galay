# Release Notes

## v0.3.0 - 2026-05-23

- **版本级别**: minor
- **Git 提交消息**: refactor: 重构 BatchSpanProcessor 为基于 moodycamel 的无锁队列架构
- **Git Tag**: v0.3.0

### 变更摘要

- **无锁队列重构**：BatchSpanProcessor 核心并发模型从 std::deque + std::mutex + condition_variable 全面切换为 moodycamel::ConcurrentQueue 无锁队列，移除 m_mutex / m_condition / m_exportMutex 三把锁，引入 SpanQueue（无锁批量出入队）和 WorkerControl（原子信号通道 + counting_semaphore）
- **唤醒策略**：新增 BatchSpanScheduleMode 枚举，支持 kTimed（纯定时）、kOnEnd（每次入队唤醒）、kBatchSize（达到批量阈值唤醒）三种策略，默认 kBatchSize
- **接口语义强化**：onEnd 签名从 `Span` 改为 `Span&&`，SpanProcessor 基类同步更新
- **并发 forceFlush**：改用 binary_semaphore 请求-响应模式替代原轮询式实现，支持多线程并发 forceFlush
- **优雅关闭**：shutdown 流程重写，原子 joinClaimed 保证单线程 join，排空完成后再关闭 exporter
- **性能基准**：新增 B7-BatchProcessorSchedule 和 B8-MoodycamelBatchQueue benchmark，以及 Rust crossbeam 对比基准
- **依赖管理**：通过 CMake find_path 查找系统安装的 moodycamel ConcurrentQueue
- **文档补充**：为所有 .cc 和 .cppm 文件添加中文 Doxygen 文件级注释，新增 t7 单元测试

## v0.2.1 - 2026-05-20

## v0.2.1 - 2026-05-20

- **版本级别**: patch
- **Git 提交消息**: docs: 为所有头文件接口添加中文 Doxygen 文档注释
- **Git Tag**: v0.2.1

### 变更摘要

- 为项目所有头文件添加完整的中文 Doxygen 文档注释
- 注释覆盖文件级（@file/@brief/@author/@version/@details）、类/结构体级（@brief/@details/@tparam/@note）、方法级（@brief/@param/@return）以及成员变量和枚举值的行尾 ///< 注释
- 更新 CMakeLists.txt 版本号至 v0.2.1
