/**
 * @file runtime.h
 * @brief 管理 IO 和 compute 调度器的运行时入口
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 定义 Runtime（顶层调度器编排器）、RuntimeHandle（运行时上下文轻量访问器）、
 * RuntimeConfig（构造参数）和 RuntimeBuilder（流式配置 API）。
 *
 * Runtime 持有 IO 调度器、compute 调度器和阻塞执行器，
 * 支持 IO 调度器间的工作窃取和 CPU 亲和性绑定。
 */

#ifndef GALAY_KERNEL_RUNTIME_H
#define GALAY_KERNEL_RUNTIME_H

#include "../common/kernel_config.h"

#include "blocking_executor.h"
#include "task.h"
#include "../parallel/parallel_scheduler.h"
#include "io_scheduler.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <span>
#include <vector>

namespace galay::kernel
{

/**
 * @brief Runtime 的绑核配置。
 *
 * @note
 * - `Mode::None` 表示不主动绑核
 * - `Mode::Sequential` 按 0..N-1 顺序分配 CPU
 * - `Mode::Custom` 要求调用方提供与 scheduler 数量完全一致的 CPU 列表
 */
struct RuntimeAffinityConfig {
    std::vector<uint32_t> custom_io_cpus;  ///< Custom 模式下 IO scheduler 的目标 CPU 列表
    std::vector<uint32_t> custom_parallel_cpus;  ///< Custom 模式下 parallel scheduler 的目标 CPU 列表
    size_t seq_io_count = 0;  ///< Sequential 模式下参与分配的 IO scheduler 数
    size_t seq_parallel_count = 0;  ///< Sequential 模式下参与分配的 parallel scheduler 数
    enum class Mode { None, Sequential, Custom } mode = Mode::None;  ///< 绑核分配模式
};

/**
 * @brief Runtime 的构建配置。
 *
 * 当 `*_scheduler_count` 为 `GALAY_RUNTIME_SCHEDULER_COUNT_AUTO` 时，
 * Runtime 会在首次启动时按当前机器 CPU 数自动创建默认调度器。
 */
struct RuntimeConfig {
    size_t io_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;  ///< IO scheduler 数；AUTO 表示按 CPU 自动推导
    size_t parallel_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;  ///< parallel scheduler 数；AUTO 表示按 CPU 自动推导
    RuntimeAffinityConfig affinity;  ///< Runtime 的绑核策略
};

/**
 * @brief Runtime 级别的调度统计快照
 * @details 当前只暴露 IO scheduler 的 work-stealing 计数。
 */
struct RuntimeStats {
    std::vector<IOSchedulerStealStats> io_schedulers;  ///< 与 get_io_scheduler(i) 对齐的 stealing 统计
};

/**
 * @brief Runtime API 的错误类别。
 *
 * 这些错误只描述提交、上下文和根任务取结果阶段的失败；具体 I/O 操作错误仍由
 * 对应 awaitable 的 `std::expected<..., IOError>` 返回。
 */
enum class RuntimeErrorCode : uint8_t {
    kNoSchedulerAvailable,  ///< runtime 中没有可用于根任务的 scheduler
    kSubmitFailed,          ///< 根任务提交到 scheduler 失败
    kNoCurrentRuntime,      ///< 当前线程未绑定 Runtime 上下文
    kInvalidHandle,         ///< RuntimeHandle 未绑定有效 Runtime
    kTaskException,         ///< 根任务执行或取结果阶段产生异常
    kBlockingSubmitFailed,  ///< 阻塞线程池提交 callable 失败
    kSchedulerStartFailed,  ///< scheduler 或底层 reactor 启动失败
    kResumeFailed           ///< 根任务等待的 owner scheduler 无法恢复
};

/**
 * @brief Runtime API 的错误对象。
 *
 * @details
 * `RuntimeError` 用于 `Runtime` 和 `RuntimeHandle` 的 `std::expected` 错误分支。
 */
class RuntimeError
{
public:
    explicit RuntimeError(RuntimeErrorCode error_code) noexcept
        : m_code(error_code)
    {
    }

    /**
     * @brief 返回 Runtime 错误类别
     * @return 当前对象的类型、状态或错误码
     */
    RuntimeErrorCode code() const noexcept { return m_code; }
    std::string_view message() const noexcept
    {
        static constexpr std::array<std::string_view, static_cast<size_t>(RuntimeErrorCode::kResumeFailed) + 1> kMessages = {{
            "runtime has no scheduler available for task execution",
            "runtime failed to submit the task to its scheduler",
            "current thread is not running inside a runtime context",
            "runtime handle is not bound to a runtime",
            "root task completed with an unhandled exception",
            "runtime failed to submit the blocking task",
            "runtime failed to start a scheduler",
            "runtime could not resume a task on its owner scheduler"
        }};

        const auto index = static_cast<size_t>(m_code);
        if (index < kMessages.size()) {
            return kMessages[index];
        }
        return "unknown runtime error";
    }

private:
    RuntimeErrorCode m_code;
};

class RuntimeHandle;

/**
 * @brief 运行时入口，负责管理 IO / parallel scheduler 与阻塞线程池。
 *
 * `Runtime` 在首次提交任务或显式 start() 时按配置创建内置
 * scheduler。实例本身不可拷贝；生命周期结束时会调用 `stop()` 停止其管理的调度器。
 */
class Runtime
{
public:
    /**
     * @brief 用给定配置构造 Runtime，尚未启动
     * @param config 配置对象
     */
    explicit Runtime(const RuntimeConfig& config = RuntimeConfig{});
    ~Runtime();  ///< 析构时停止所有受管 scheduler 和阻塞执行器

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    /**
     * @brief 启动 runtime 及其管理的 scheduler。
     *
     * 按 `RuntimeConfig` 创建内置实例；不支持自定义调度器注入。
     * 重复调用安全，已运行时直接返回。
     * @return 成功时返回空值，失败时返回 RuntimeError 错误
     */
    std::expected<void, RuntimeError> start();

    /**
     * @brief 停止 runtime 及其管理的 scheduler。
     *
     * 停止顺序为 blocking -> compute -> IO -> timer；重复调用安全。
     * @return 无返回值
     */
    void stop();

    /**
     * @brief 在 IO scheduler 上同步执行一个根任务并返回结果。
     * @param task 协程任务
     * @return 成功时返回 T，失败时返回 RuntimeError 错误
     */
    template <typename T>
    auto block_on_io(Task<T> task) -> std::expected<T, RuntimeError>
    {
        return block_on_on_scheduler(std::move(task), acquire_io_scheduler());
    }

    /**
     * @brief 在 parallel scheduler 上同步执行一个纯计算根任务并返回结果。
     * @param task 协程任务
     * @return 成功时返回 T，失败时返回 RuntimeError 错误
     */
    template <typename T>
    auto block_on_cpu(Task<T> task) -> std::expected<T, RuntimeError>
    {
        return block_on_on_scheduler(std::move(task), acquire_parallel_scheduler());
    }

private:
    template <typename T, typename SchedulerT>
    auto block_on_on_scheduler(Task<T> task, std::expected<SchedulerT*, RuntimeError> scheduler)
        -> std::expected<T, RuntimeError>
    {
        if (!scheduler.has_value()) {
            return std::unexpected(scheduler.error());
        }
        if (*scheduler == nullptr) {
            return std::unexpected(RuntimeError(RuntimeErrorCode::kNoSchedulerAvailable));
        }

        const TaskRef& task_ref = detail::TaskAccess::task_ref(task);
        bind_task_to_runtime(task_ref, *scheduler);
        if (!(*scheduler)->schedule(task_ref)) {
            return std::unexpected(RuntimeError(RuntimeErrorCode::kSubmitFailed));
        }

        auto result = detail::TaskAccess::try_take_result(task);
        if (!result.has_value()) {
            return std::unexpected(map_task_result_error(result.error()));
        }
        if constexpr (std::is_void_v<T>) {
            return {};
        } else {
            return std::move(*result);
        }
    }

public:
    /**
     * @brief 在 IO scheduler 上异步提交一个任务。
     * @param task 要提交的任务；所有权转移到 runtime
     * @return 成功时返回可 `join()` 的句柄；没有 IO scheduler 或提交失败时返回 RuntimeError
     */
    template <typename T>
    auto spawn_io(Task<T> task) -> std::expected<JoinHandle<T>, RuntimeError>
    {
        return spawn_on_scheduler(std::move(task), acquire_io_scheduler());
    }

    /**
     * @brief 在 parallel scheduler 上异步提交一个纯计算任务。
     * @param task 要提交的任务；任务不得依赖 IO scheduler 专属事件循环
     * @return 成功时返回可 `join()` 的句柄；没有 parallel scheduler 或提交失败时返回 RuntimeError
     */
    template <typename T>
    auto spawn_cpu(Task<T> task) -> std::expected<JoinHandle<T>, RuntimeError>
    {
        return spawn_on_scheduler(std::move(task), acquire_parallel_scheduler());
    }

private:
    template <typename T, typename SchedulerT>
    auto spawn_on_scheduler(Task<T> task, std::expected<SchedulerT*, RuntimeError> scheduler)
        -> std::expected<JoinHandle<T>, RuntimeError>
    {
        if (!scheduler.has_value()) {
            return std::unexpected(scheduler.error());
        }
        if (*scheduler == nullptr) {
            return std::unexpected(RuntimeError(RuntimeErrorCode::kNoSchedulerAvailable));
        }

        const TaskRef& task_ref = detail::TaskAccess::task_ref(task);
        bind_task_to_runtime(task_ref, *scheduler);
        if (!(*scheduler)->schedule(task_ref)) {
            return std::unexpected(RuntimeError(RuntimeErrorCode::kSubmitFailed));
        }
        return JoinHandle<T>(detail::TaskAccess::detach_task(std::move(task)));
    }

public:
    /**
     * @brief 在线程池上执行一个阻塞 callable，并返回 join handle。
     * @param func 可调用对象；会被 move/copy 进阻塞线程池
     * @return 成功时返回 `JoinHandle<Result>`；提交阻塞任务失败时返回 RuntimeError
     *
     * @note
     * - 适合文件阻塞 IO、第三方同步库调用等不可协程化路径
     * - callable 内部会继承当前 runtime 上下文，因此可安全调用 `RuntimeHandle::try_current()`
     * - callable 需要通过返回值表达业务失败；提交失败由 `RuntimeError` 返回
     */
    template <typename F>
    auto spawn_blocking(F&& func) -> std::expected<JoinHandle<std::invoke_result_t<std::decay_t<F>&>>, RuntimeError>
    {
        using Fn = std::decay_t<F>;
        using Result = std::invoke_result_t<Fn&>;

        auto completion = std::make_shared<TaskCompletionState<Result>>();
        auto submitted = m_blockingExecutor.submit([runtime = this, completion, function = Fn(std::forward<F>(func))]() mutable {
            detail::CurrentRuntimeScope runtime_scope(runtime);
            try {
                if constexpr (std::is_void_v<Result>) {
                    std::invoke(function);
                    completion->set_value();
                } else {
                    completion->set_value(std::invoke(function));
                }
            } catch (...) {
                completion->set_error(detail::TaskResultError(detail::TaskResultErrorCode::kTaskException));
            }
        });
        if (!submitted.has_value()) {
            return std::unexpected(RuntimeError(RuntimeErrorCode::kBlockingSubmitFailed));
        }

        return JoinHandle<Result>(std::move(completion));
    }

    /**
     * @brief 获取一个轻量 `RuntimeHandle`，用于把当前 runtime 传递到其他层。
     * @return 关联当前运行时的轻量句柄
     */
    RuntimeHandle handle() noexcept;
    /**
     * @brief 返回 Runtime 管理的 scheduler 统计；应在 stop() 后或外部同步下调用
     * @return RuntimeStats 操作结果
     */
    RuntimeStats stats() const;

    /**
     * @brief Runtime 当前是否已启动
     * @return 满足所检查条件时返回 true，否则返回 false
     */
    bool is_running() const { return m_running.load(std::memory_order_acquire); }
    /**
     * @brief 返回当前受管 IO scheduler 数量
     * @return 对应的大小或数量
     */
    size_t get_io_scheduler_count() const { return m_io_schedulers.size(); }
    /**
     * @brief 返回当前受管 parallel scheduler 数量
     * @return 对应的大小或数量
     */
    size_t get_parallel_scheduler_count() const { return m_parallel_schedulers.size(); }

    /**
     * @brief 按索引返回 IO scheduler；越界时返回 nullptr
     * @param index 元素索引
     * @return IOScheduler* 指针
     */
    IOScheduler* get_io_scheduler(size_t index);
    /**
     * @brief 按索引返回 parallel scheduler；越界时返回 nullptr
     * @param index 元素索引
     * @return ParallelScheduler* 指针
     */
    ParallelScheduler* get_parallel_scheduler(size_t index);
    /**
     * @brief 以轮询方式返回下一个 IO scheduler；不存在时返回 nullptr
     * @return IOScheduler* 指针
     */
    IOScheduler* get_next_io_scheduler();
    /**
     * @brief 以轮询方式返回下一个 parallel scheduler；不存在时返回 nullptr
     * @return ParallelScheduler* 指针
     */
    ParallelScheduler* get_next_parallel_scheduler();

private:
    /**
     * @brief 按配置或 CPU 数生成默认 scheduler 集合
     * @return 无返回值
     */
    void create_default_schedulers();
    /**
     * @brief 把 RuntimeAffinityConfig 应用到所有已注册 scheduler
     * @return 无返回值
     */
    void apply_affinity_config();
    /**
     * @brief 若 Runtime 尚未启动则触发一次启动
     * @return 成功时返回空值，失败时返回 RuntimeError 错误
     */
    std::expected<void, RuntimeError> ensure_started();
    /**
     * @brief 保留 IO 根任务提交时的具体调度器类型
     * @return 成功时返回 IOScheduler*，失败时返回 RuntimeError 错误
     */
    std::expected<IOScheduler*, RuntimeError> acquire_io_scheduler();
    /**
     * @brief 保留 CPU 根任务提交时的具体调度器类型
     * @return 成功时返回 ParallelScheduler*，失败时返回 RuntimeError 错误
     */
    std::expected<ParallelScheduler*, RuntimeError> acquire_parallel_scheduler();
    /**
     * @brief 给根任务绑定 Runtime 与目标调度器
     * @param task 协程任务
     * @param scheduler 执行异步操作的 IO 调度器
     * @return 无返回值
     */
    void bind_task_to_runtime(const TaskRef& task, Scheduler* scheduler);
    /**
     * @brief 返回当前机器可用 CPU 数量
     * @return 对应的大小或数量
     */
    static size_t get_cpu_count();
    /**
     * @brief 把任务消费错误映射为 RuntimeError
     * @param error 错误信息
     * @return RuntimeError 操作结果
     */
    static RuntimeError map_task_result_error(const detail::TaskResultError& error) noexcept;
    /**
     * @brief 为 Runtime 管理的 IO scheduler 下发 steal-domain 配置
     * @return 无返回值
     */
    void configure_io_scheduler_steal_domains();

    std::vector<std::unique_ptr<IOScheduler>> m_io_schedulers;  ///< Runtime 持有的 IO scheduler 集合
    std::vector<std::unique_ptr<ParallelScheduler>> m_parallel_schedulers;  ///< Runtime 持有的 parallel scheduler 集合

    std::vector<IOScheduler*> m_io_scheduler_sibling_view;  ///< Runtime 管理的 IO scheduler pointer view

    std::atomic<uint32_t> m_io_index{0};  ///< IO scheduler 轮询游标
    std::atomic<uint32_t> m_parallel_index{0};  ///< parallel scheduler 轮询游标

    BlockingExecutor m_blockingExecutor;  ///< 阻塞任务线程池
    RuntimeConfig m_config;  ///< Runtime 启动和绑核配置
    std::atomic<bool> m_running{false};  ///< Runtime 是否已经启动
};

/**
 * @brief Runtime 的轻量句柄
 * @details 用于在协程或阻塞线程池回调中访问当前 Runtime，而不暴露所有权。
 */
class RuntimeHandle
{
public:
    RuntimeHandle() noexcept = default;  ///< 构造空 handle
    explicit RuntimeHandle(Runtime* runtime) noexcept
        : m_runtime(runtime)
    {
    }

    /**
     * @brief 获取当前线程绑定的 runtime handle。
     * @return 成功时返回当前 RuntimeHandle；不在 runtime 上下文中时返回 RuntimeError
     */
    static std::expected<RuntimeHandle, RuntimeError> current();

    /**
     * @brief 尝试获取当前线程绑定的 runtime handle。
     * @return 当前 runtime 存在时返回值，否则返回 `std::nullopt`
     */
    static std::optional<RuntimeHandle> try_current();

    /**
     * @brief 当前是否绑定到有效 Runtime
     * @return 满足所检查条件时返回 true，否则返回 false
     */
    bool is_valid() const noexcept { return m_runtime != nullptr; }

    /**
     * @brief 通过当前 runtime 在 IO scheduler 上提交任务。
     * @param task 要提交的任务；所有权转移到 runtime
     * @return 成功时返回可 `join()` 的句柄；handle 无效、没有 IO scheduler 或提交失败时返回 RuntimeError
     */
    template <typename T>
    auto spawn_io(Task<T> task) const -> std::expected<JoinHandle<T>, RuntimeError>
    {
        auto runtime = runtime_or_error();
        if (!runtime.has_value()) {
            return std::unexpected(runtime.error());
        }
        return (*runtime)->spawn_io(std::move(task));
    }

    /**
     * @brief 通过当前 runtime 在 parallel scheduler 上提交纯计算任务。
     * @param task 要提交的任务；任务不得依赖 IO scheduler 专属事件循环
     * @return 成功时返回可 `join()` 的句柄；handle 无效、没有 parallel scheduler 或提交失败时返回 RuntimeError
     */
    template <typename T>
    auto spawn_cpu(Task<T> task) const -> std::expected<JoinHandle<T>, RuntimeError>
    {
        auto runtime = runtime_or_error();
        if (!runtime.has_value()) {
            return std::unexpected(runtime.error());
        }
        return (*runtime)->spawn_cpu(std::move(task));
    }

    template <typename F>
    auto spawn_blocking(F&& func) const -> std::expected<JoinHandle<std::invoke_result_t<std::decay_t<F>&>>, RuntimeError>
    {
        auto runtime = runtime_or_error();
        if (!runtime.has_value()) {
            return std::unexpected(runtime.error());
        }
        return (*runtime)->spawn_blocking(std::forward<F>(func));
    }

private:
    std::expected<Runtime*, RuntimeError> runtime_or_error() const noexcept  ///< 返回绑定的 Runtime；未绑定时返回 RuntimeError
    {
        if (m_runtime == nullptr) {
            return std::unexpected(RuntimeError(RuntimeErrorCode::kInvalidHandle));
        }
        return m_runtime;
    }

    Runtime* m_runtime = nullptr;  ///< 关联的 Runtime，生命周期由外部持有
};

class RuntimeBuilder
{
public:
    /**
     * @brief 设置 IO scheduler 数量。
     * @param n 数量
     * @return 当前对象引用
     * @note 传 `GALAY_RUNTIME_SCHEDULER_COUNT_AUTO` 时由 runtime 按 CPU 数自动推导
     */
    RuntimeBuilder& io_scheduler_count(size_t n)
    {
        m_config.io_scheduler_count = n;
        return *this;
    }

    /**
     * @brief 设置 parallel scheduler 数量。
     * @param n 数量
     * @return 当前对象引用
     * @note 传 `GALAY_RUNTIME_SCHEDULER_COUNT_AUTO` 时由 runtime 按 CPU 数自动推导
     */
    RuntimeBuilder& parallel_scheduler_count(size_t n)
    {
        m_config.parallel_scheduler_count = n;
        return *this;
    }

    /**
     * @brief 对前 `ioCount` / `parallelCount` 个 scheduler 依次分配 CPU 亲和性。
     * @param ioCount IO 调度器数量
     * @param parallelCount 并行调度器数量
     * @return 当前对象引用
     */
    RuntimeBuilder& sequential_affinity(size_t ioCount, size_t parallelCount)
    {
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Sequential;
        m_config.affinity.seq_io_count = ioCount;
        m_config.affinity.seq_parallel_count = parallelCount;
        return *this;
    }

    /**
     * @brief 为每个 scheduler 指定显式 CPU 亲和性列表。
     * @param ioCpus IO 调度器绑定的 CPU 集合
     * @param parallelCpus 并行调度器绑定的 CPU 集合
     * @return 列表长度与当前 scheduler 配置完全匹配时返回 `true`
     */
    bool custom_affinity(std::vector<uint32_t> ioCpus, std::vector<uint32_t> parallelCpus)
    {
        if (ioCpus.size() != m_config.io_scheduler_count ||
            parallelCpus.size() != m_config.parallel_scheduler_count) {
            return false;
        }
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Custom;
        m_config.affinity.custom_io_cpus = std::move(ioCpus);
        m_config.affinity.custom_parallel_cpus = std::move(parallelCpus);
        return true;
    }

    /**
     * @brief 直接覆盖完整 affinity 配置。
     * @param affinity CPU 亲和性配置
     * @return 当前对象引用
     */
    RuntimeBuilder& apply_affinity(const RuntimeAffinityConfig& affinity)
    {
        m_config.affinity = affinity;
        return *this;
    }

    /**
     * @brief 按当前 builder 配置构造 `Runtime`。
     * @return 按当前配置创建的 Runtime 对象
     */
    Runtime build() const { return Runtime(m_config); }

    /**
     * @brief 导出当前 builder 累积的配置快照。
     * @return 当前构建器的配置快照
     */
    RuntimeConfig build_config() const { return m_config; }

private:
    RuntimeConfig m_config;
};

} // namespace galay::kernel

#include "scheduler_dispatch.hpp"

#endif // GALAY_KERNEL_RUNTIME_H
