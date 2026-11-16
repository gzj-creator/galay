/**
 * @file io_scheduler_adapter.hpp
 * @brief 三个 IO 后端共用的调度器适配模板。
 * @details IOSchedulerBase 负责调度与任务排空；本层参数化后端类别，集中实现
 * poll/flush 的生命周期适配，并为配置类型提供统一的事件容量或队列深度入口。
 */
#ifndef GALAY_KERNEL_IO_SCHEDULER_ADAPTER_HPP
#define GALAY_KERNEL_IO_SCHEDULER_ADAPTER_HPP

#include "io_scheduler_base.hpp"
#include "../common/scheduler_config.h"

#include <cstddef>
#include <cstdint>
#include <ctime>

namespace galay::kernel::detail {

/** @brief 调度器适配层区分的 poll/flush 后端类别。 */
enum class IOSchedulerBackendKind {
    kEpoll,
    kKqueue,
    kIOUring,
};

/**
 * @brief 参数化后端调度器的生命周期与 poll/flush 适配。
 * @tparam Derived 对外的具体后端类型，供 Scheduler 静态分派使用
 * @tparam Reactor 平台 reactor
 * @tparam Kind 后端类别，仅决定本层调用哪个 poll 超时接口
 */
template <
    typename Derived,
    typename Reactor,
    IOSchedulerBackendKind Kind
>
class IOSchedulerAdapter : public IOSchedulerBase<Derived, Reactor>
{
    using Base = IOSchedulerBase<Derived, Reactor>;

protected:
    explicit IOSchedulerAdapter(int reactor_capacity, int batch_size)
        : Base(reactor_capacity, batch_size)
    {
    }

    ~IOSchedulerAdapter()
    {
        this->stop();
    }

    using Base::m_reactor;
    using Base::m_wake_coordinator;
    using Base::scheduler_poll_timeout_io_uring_nanoseconds;
    using Base::scheduler_poll_timeout_milliseconds;
    using Base::scheduler_poll_timeout_nanoseconds;

private:
    friend class IOSchedulerBase<Derived, Reactor>;

    void poll_backend()
    {
        if constexpr (Kind == IOSchedulerBackendKind::kEpoll) {
            m_reactor.poll(scheduler_poll_timeout_milliseconds(), m_wake_coordinator);
        } else if constexpr (Kind == IOSchedulerBackendKind::kKqueue) {
            const uint64_t ns = scheduler_poll_timeout_nanoseconds();
            const timespec timeout{
                .tv_sec = static_cast<::time_t>(ns / 1'000'000'000ULL),
                .tv_nsec = static_cast<long>(ns % 1'000'000'000ULL),
            };
            m_reactor.poll(timeout, m_wake_coordinator);
        } else {
            m_reactor.poll(
                scheduler_poll_timeout_io_uring_nanoseconds(), m_wake_coordinator);
        }
    }

    void flush_backend()
    {
        if constexpr (Kind != IOSchedulerBackendKind::kIOUring) {
            // flush 将错误保存到 last_error；事件循环继续排空已接纳的任务。
            (void)m_reactor.flush_pending_changes();
        }
    }
};

/** @brief epoll/kqueue 配置只向共享后端传递最大事件数。 */
template <typename Backend, typename Config>
class IOSchedulerMaxEventsAdapter : public Backend
{
public:
    using ConfigType = Config;

    explicit IOSchedulerMaxEventsAdapter()
        : Backend(static_cast<int>(Config::kMaxEvents),
                  static_cast<int>(Config::kBatchSize))
    {
    }

    IOSchedulerMaxEventsAdapter(const IOSchedulerMaxEventsAdapter&) = delete;
    IOSchedulerMaxEventsAdapter& operator=(const IOSchedulerMaxEventsAdapter&) = delete;

    static constexpr size_t max_events() noexcept { return Config::kMaxEvents; }
    static constexpr size_t batch_size() noexcept { return Config::kBatchSize; }
};

/** @brief io_uring 配置只向共享后端传递提交/完成队列深度。 */
template <typename Backend, typename Config>
class IOSchedulerQueueDepthAdapter : public Backend
{
public:
    using ConfigType = Config;

    explicit IOSchedulerQueueDepthAdapter()
        : Backend(static_cast<int>(Config::kQueueDepth),
                  static_cast<int>(Config::kBatchSize))
    {
    }

    IOSchedulerQueueDepthAdapter(const IOSchedulerQueueDepthAdapter&) = delete;
    IOSchedulerQueueDepthAdapter& operator=(const IOSchedulerQueueDepthAdapter&) = delete;

    static constexpr size_t queue_depth() noexcept { return Config::kQueueDepth; }
    static constexpr size_t batch_size() noexcept { return Config::kBatchSize; }
};

} // namespace galay::kernel::detail

#endif
