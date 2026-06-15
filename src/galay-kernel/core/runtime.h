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

#include "blocking_executor.h"
#include "task.h"
#include "compute_scheduler.h"
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

#define GALAY_RUNTIME_SCHEDULER_COUNT_AUTO static_cast<size_t>(-1)  ///< 自动按 CPU 数量推导 scheduler 个数

/**
 * @brief Runtime 的绑核配置。
 *
 * @note
 * - `Mode::None` 表示不主动绑核
 * - `Mode::Sequential` 按 0..N-1 顺序分配 CPU
 * - `Mode::Custom` 要求调用方提供与 scheduler 数量完全一致的 CPU 列表
 */
struct RuntimeAffinityConfig {
    enum class Mode { None, Sequential, Custom } mode = Mode::None;  ///< 绑核分配模式
    size_t seq_io_count = 0;  ///< Sequential 模式下参与分配的 IO scheduler 数
    size_t seq_compute_count = 0;  ///< Sequential 模式下参与分配的 compute scheduler 数
    std::vector<uint32_t> custom_io_cpus;  ///< Custom 模式下 IO scheduler 的目标 CPU 列表
    std::vector<uint32_t> custom_compute_cpus;  ///< Custom 模式下 compute scheduler 的目标 CPU 列表
};

/**
 * @brief Runtime 的构建配置。
 *
 * 当 `*_scheduler_count` 为 `GALAY_RUNTIME_SCHEDULER_COUNT_AUTO` 时，
 * Runtime 会在首次启动时按当前机器 CPU 数自动创建默认调度器。
 */
struct RuntimeConfig {
    size_t io_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;  ///< IO scheduler 数；AUTO 表示按 CPU 自动推导
    size_t compute_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO;  ///< compute scheduler 数；AUTO 表示按 CPU 自动推导
    RuntimeAffinityConfig affinity;  ///< Runtime 的绑核策略
};

/**
 * @brief Runtime 级别的调度统计快照
 * @details 当前只暴露 IO scheduler 的 work-stealing 计数。
 */
struct RuntimeStats {
    std::vector<IOSchedulerStealStats> io_schedulers;  ///< 与 getIOScheduler(i) 对齐的 stealing 统计
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
    kBlockingSubmitFailed   ///< 阻塞线程池提交 callable 失败
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

    RuntimeErrorCode code() const noexcept { return m_code; }  ///< 返回 Runtime 错误类别
    std::string_view message() const noexcept
    {
        static constexpr std::array<std::string_view, static_cast<size_t>(RuntimeErrorCode::kBlockingSubmitFailed) + 1> kMessages = {{
            "runtime has no scheduler available for task execution",
            "runtime failed to submit the task to its scheduler",
            "current thread is not running inside a runtime context",
            "runtime handle is not bound to a runtime",
            "root task completed with an unhandled exception",
            "runtime failed to submit the blocking task"
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
 * @brief 运行时入口，负责管理 IO / compute scheduler 与阻塞线程池。
 *
 * `Runtime` 可以显式注入 scheduler，也可以在首次提交任务时按配置自动创建默认
 * scheduler。实例本身不可拷贝；生命周期结束时会调用 `stop()` 停止其管理的调度器。
 */
class Runtime
{
public:
    explicit Runtime(const RuntimeConfig& config = RuntimeConfig{});  ///< 用给定配置构造 Runtime，尚未启动
    ~Runtime();  ///< 析构时停止所有受管 scheduler 和阻塞执行器

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    /**
     * @brief 在 runtime 启动前注册一个 IO scheduler。
     * @return 启动前返回 `true`；若 runtime 已运行则返回 `false`
     */
    bool addIOScheduler(std::unique_ptr<IOScheduler> scheduler);

    /**
     * @brief 在 runtime 启动前注册一个 compute scheduler。
     * @return 启动前返回 `true`；若 runtime 已运行则返回 `false`
     */
    bool addComputeScheduler(std::unique_ptr<ComputeScheduler> scheduler);

    /**
     * @brief 启动 runtime 及其管理的 scheduler。
     *
     * 若未显式注册 scheduler，会按 `RuntimeConfig` 自动创建默认实例。
     * 重复调用安全，已运行时直接返回。
     */
    void start();

    /**
     * @brief 停止 runtime 及其管理的 scheduler。
     *
     * 停止顺序为 compute -> IO -> timer；重复调用安全。
     */
    void stop();

    /**
     * @brief 在 runtime 上提交一个根任务并同步等待结果。
     * @param task 要提交的任务；所有权转移到 runtime
     * @return 成功时返回任务结果；`Task<void>` 成功时返回 `std::expected<void, RuntimeError>{}`；
     *         失败时返回 RuntimeError
     *
     * @note 若 runtime 尚未启动，会在内部自动启动
     */
    template <typename T>
    auto blockOn(Task<T> task) -> std::expected<T, RuntimeError>
    {
        Scheduler* scheduler = acquireDefaultScheduler();
        if (scheduler == nullptr) {
            return std::unexpected(RuntimeError(RuntimeErrorCode::kNoSchedulerAvailable));
        }

        const TaskRef& taskRef = detail::TaskAccess::taskRef(task);
        bindTaskToRuntime(taskRef, scheduler);
        if (!submitTask(taskRef)) {
            return std::unexpected(RuntimeError(RuntimeErrorCode::kSubmitFailed));
        }

        auto result = detail::TaskAccess::tryTakeResult(task);
        if (!result.has_value()) {
            return std::unexpected(mapTaskResultError(result.error()));
        }
        if constexpr (std::is_void_v<T>) {
            return {};
        } else {
            return std::move(*result);
        }
    }

    /**
     * @brief 异步提交一个任务并返回可 `join()` 的句柄。
     * @param task 要提交的任务；所有权转移到 runtime
     * @return 成功时返回与任务结果绑定的 `JoinHandle<T>`，失败时返回 RuntimeError
     *
     * @note 若 runtime 尚未启动，会在内部自动启动
     */
    template <typename T>
    auto spawn(Task<T> task) -> std::expected<JoinHandle<T>, RuntimeError>
    {
        Scheduler* scheduler = acquireDefaultScheduler();
        if (scheduler == nullptr) {
            return std::unexpected(RuntimeError(RuntimeErrorCode::kNoSchedulerAvailable));
        }

        const TaskRef& taskRef = detail::TaskAccess::taskRef(task);
        bindTaskToRuntime(taskRef, scheduler);
        if (!submitTask(taskRef)) {
            return std::unexpected(RuntimeError(RuntimeErrorCode::kSubmitFailed));
        }
        return JoinHandle<T>(detail::TaskAccess::detachTask(std::move(task)));
    }

    /**
     * @brief 在线程池上执行一个阻塞 callable，并返回 join handle。
     * @param func 可调用对象；会被 move/copy 进阻塞线程池
     * @return 成功时返回 `JoinHandle<Result>`；提交阻塞任务失败时返回 RuntimeError
     *
     * @note
     * - 适合文件阻塞 IO、第三方同步库调用等不可协程化路径
     * - callable 内部会继承当前 runtime 上下文，因此可安全调用 `RuntimeHandle::tryCurrent()`
     * - callable 需要通过返回值表达业务失败；提交失败由 `RuntimeError` 返回
     */
    template <typename F>
    auto spawnBlocking(F&& func) -> std::expected<JoinHandle<std::invoke_result_t<std::decay_t<F>&>>, RuntimeError>
    {
        using Fn = std::decay_t<F>;
        using Result = std::invoke_result_t<Fn&>;

        auto completion = std::make_shared<TaskCompletionState<Result>>();
        auto submitted = m_blockingExecutor.submit([runtime = this, completion, function = Fn(std::forward<F>(func))]() mutable {
            detail::CurrentRuntimeScope runtimeScope(runtime);
            if constexpr (std::is_void_v<Result>) {
                std::invoke(function);
                completion->setValue();
            } else {
                completion->setValue(std::invoke(function));
            }
        });
        if (!submitted.has_value()) {
            return std::unexpected(RuntimeError(RuntimeErrorCode::kBlockingSubmitFailed));
        }

        return JoinHandle<Result>(std::move(completion));
    }

    /**
     * @brief 获取一个轻量 `RuntimeHandle`，用于把当前 runtime 传递到其他层。
     */
    RuntimeHandle handle() noexcept;
    RuntimeStats stats() const;  ///< 返回 Runtime 管理的 scheduler 统计；应在 stop() 后或外部同步下调用

    bool isRunning() const { return m_running.load(std::memory_order_acquire); }  ///< Runtime 当前是否已启动
    size_t getIOSchedulerCount() const { return m_io_schedulers.size(); }  ///< 返回当前受管 IO scheduler 数量
    size_t getComputeSchedulerCount() const { return m_compute_schedulers.size(); }  ///< 返回当前受管 compute scheduler 数量

    IOScheduler* getIOScheduler(size_t index);  ///< 按索引返回 IO scheduler；越界时返回 nullptr
    ComputeScheduler* getComputeScheduler(size_t index);  ///< 按索引返回 compute scheduler；越界时返回 nullptr
    IOScheduler* getNextIOScheduler();  ///< 以轮询方式返回下一个 IO scheduler；不存在时返回 nullptr
    ComputeScheduler* getNextComputeScheduler();  ///< 以轮询方式返回下一个 compute scheduler；不存在时返回 nullptr

private:
    void createDefaultSchedulers();  ///< 按配置或 CPU 数生成默认 scheduler 集合
    void applyAffinityConfig();  ///< 把 RuntimeAffinityConfig 应用到所有已注册 scheduler
    void ensureStarted();  ///< 若 Runtime 尚未启动则触发一次启动
    Scheduler* acquireDefaultScheduler();  ///< 为根任务选出一个默认调度器
    void bindTaskToRuntime(const TaskRef& task, Scheduler* scheduler);  ///< 给根任务绑定 Runtime 与目标调度器
    bool submitTask(const TaskRef& task);  ///< 把根任务提交到其所属调度器
    static size_t getCPUCount();  ///< 返回当前机器可用 CPU 数量
    static RuntimeError mapTaskResultError(const detail::TaskResultError& error) noexcept;  ///< 把任务消费错误映射为 RuntimeError
    void configureIOSchedulerStealDomains();  ///< 为 Runtime 管理的 IO scheduler 下发 steal-domain 配置

    std::vector<std::unique_ptr<IOScheduler>> m_io_schedulers;  ///< Runtime 持有的 IO scheduler 集合
    std::vector<std::unique_ptr<ComputeScheduler>> m_compute_schedulers;  ///< Runtime 持有的 compute scheduler 集合

    std::vector<IOScheduler*> m_io_scheduler_sibling_view;  ///< Runtime 管理的 IO scheduler pointer view

    std::atomic<uint32_t> m_io_index{0};  ///< IO scheduler 轮询游标
    std::atomic<uint32_t> m_compute_index{0};  ///< compute scheduler 轮询游标

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
    static std::optional<RuntimeHandle> tryCurrent();

    bool isValid() const noexcept { return m_runtime != nullptr; }  ///< 当前是否绑定到有效 Runtime

    template <typename T>
    auto spawn(Task<T> task) const -> std::expected<JoinHandle<T>, RuntimeError>
    {
        auto runtime = runtimeOrError();
        if (!runtime.has_value()) {
            return std::unexpected(runtime.error());
        }
        return (*runtime)->spawn(std::move(task));
    }

    template <typename F>
    auto spawnBlocking(F&& func) const -> std::expected<JoinHandle<std::invoke_result_t<std::decay_t<F>&>>, RuntimeError>
    {
        auto runtime = runtimeOrError();
        if (!runtime.has_value()) {
            return std::unexpected(runtime.error());
        }
        return (*runtime)->spawnBlocking(std::forward<F>(func));
    }

private:
    std::expected<Runtime*, RuntimeError> runtimeOrError() const noexcept  ///< 返回绑定的 Runtime；未绑定时返回 RuntimeError
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
     * @note 传 `GALAY_RUNTIME_SCHEDULER_COUNT_AUTO` 时由 runtime 按 CPU 数自动推导
     */
    RuntimeBuilder& ioSchedulerCount(size_t n)
    {
        m_config.io_scheduler_count = n;
        return *this;
    }

    /**
     * @brief 设置 compute scheduler 数量。
     * @note 传 `GALAY_RUNTIME_SCHEDULER_COUNT_AUTO` 时由 runtime 按 CPU 数自动推导
     */
    RuntimeBuilder& computeSchedulerCount(size_t n)
    {
        m_config.compute_scheduler_count = n;
        return *this;
    }

    /**
     * @brief 对前 `ioCount` / `computeCount` 个 scheduler 依次分配 CPU 亲和性。
     */
    RuntimeBuilder& sequentialAffinity(size_t ioCount, size_t computeCount)
    {
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Sequential;
        m_config.affinity.seq_io_count = ioCount;
        m_config.affinity.seq_compute_count = computeCount;
        return *this;
    }

    /**
     * @brief 为每个 scheduler 指定显式 CPU 亲和性列表。
     * @return 列表长度与当前 scheduler 配置完全匹配时返回 `true`
     */
    bool customAffinity(std::vector<uint32_t> ioCpus, std::vector<uint32_t> computeCpus)
    {
        if (ioCpus.size() != m_config.io_scheduler_count ||
            computeCpus.size() != m_config.compute_scheduler_count) {
            return false;
        }
        m_config.affinity.mode = RuntimeAffinityConfig::Mode::Custom;
        m_config.affinity.custom_io_cpus = std::move(ioCpus);
        m_config.affinity.custom_compute_cpus = std::move(computeCpus);
        return true;
    }

    /**
     * @brief 直接覆盖完整 affinity 配置。
     */
    RuntimeBuilder& applyAffinity(const RuntimeAffinityConfig& affinity)
    {
        m_config.affinity = affinity;
        return *this;
    }

    /**
     * @brief 按当前 builder 配置构造 `Runtime`。
     */
    Runtime build() const { return Runtime(m_config); }

    /**
     * @brief 导出当前 builder 累积的配置快照。
     */
    RuntimeConfig buildConfig() const { return m_config; }

private:
    RuntimeConfig m_config;
};

} // namespace galay::kernel

#endif // GALAY_KERNEL_RUNTIME_H
