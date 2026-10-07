/**
 * @file scheduler_core.h
 * @brief 调度器事件循环核心，支持 ready-pass 和工作窃取
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 实现 SchedulerCore，驱动每次迭代的任务处理周期：
 * 收集跨线程注入任务、以预算限制运行 ready pass、空闲时回退到从兄弟调度器窃取任务。
 */

#ifndef GALAY_KERNEL_SCHEDULER_CORE_H
#define GALAY_KERNEL_SCHEDULER_CORE_H

#include "io_ready_queue.hpp"

#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace galay::kernel {

/**
 * @brief 调度器主循环阶段
 * @details 供测试和诊断观察一次事件循环迭代所处的阶段。
 */
enum class SchedulerCoreStage {
    CollectRemote,       ///< 拉取跨线程注入任务
    CollectCompletions,  ///< 收集后端完成事件
    RunReady,            ///< 恢复 ready 队列中的任务
    Poll,                ///< 进入后端 poll 等待
};

/**
 * @brief 一次 ready pass 的统计结果
 */
struct SchedulerReadyPassSummary {
    size_t ran = 0;  ///< 实际恢复执行的任务数
    size_t drainedRemote = 0;  ///< 从跨线程注入队列拉取的任务数
    size_t passes = 0;  ///< 实际执行的 pass 数
};

namespace detail {

template <typename>
inline constexpr bool kUnsupportedReadyResumeCallback = false;

template <typename ResumeFn>
void invoke_ready_entry_resume(ResumeFn& resume_fn, ReadyEntry& entry)
{
    if constexpr (std::is_invocable_v<ResumeFn&, ReadyEntry&>) {
        resume_fn(entry);
    } else if constexpr (std::is_invocable_v<ResumeFn&, TaskRef&>) {
        if (!entry.is_cpp_task()) {
            (void)resume_ready_entry(entry);
            return;
        }
        TaskRef task = ready_entry_to_task_ref(entry);
        resume_fn(task);
    } else {
        static_assert(kUnsupportedReadyResumeCallback<ResumeFn>,
                      "ready pass callback must accept ReadyEntry& or TaskRef&");
    }
}

}  // namespace detail

class SchedulerCore
{
public:
    explicit SchedulerCore(IOReadyQueue& worker, size_t ready_budget) noexcept
        : m_worker(worker)
        , m_ready_budget(std::max<size_t>(1, ready_budget))
    {
    }

    void set_ready_budget(size_t ready_budget) noexcept {
        m_ready_budget = std::max<size_t>(1, ready_budget);
    }

    size_t ready_budget() const noexcept {
        return m_ready_budget;
    }

    bool has_pending_work() const noexcept {
        return m_worker.has_local_work() || m_worker.has_pending_injected();
    }

    bool try_steal() noexcept {
        return m_worker.try_steal();
    }

    template <typename OnRemoteCollectedFn>
    size_t collect_remote(OnRemoteCollectedFn&& on_remote_collected_fn) {
        if (!m_worker.has_local_work() ||
            m_worker.should_check_injected() ||
            m_worker.has_pending_injected()) {
            const size_t drained = m_worker.drain_injected();
            on_remote_collected_fn(drained);
            return drained;
        }
        return 0;
    }

    size_t collect_remote() {
        return collect_remote([](size_t) {});
    }

    template <typename ResumeFn, typename OnRemoteCollectedFn>
    SchedulerReadyPassSummary run_ready_pass_detailed(ResumeFn&& resume_fn,
                                                   OnRemoteCollectedFn&& on_remote_collected_fn) {
        const bool allow_injected_burst = !m_worker.has_local_work();
        size_t burst_credit = 0;
        SchedulerReadyPassSummary summary;
        detail::ReadyEntry next;
        auto on_remote_collected = [&](size_t drained) {
            summary.drainedRemote += drained;
            on_remote_collected_fn(drained);
        };

        while (true) {
            size_t drained = collect_remote(on_remote_collected);
            if (allow_injected_burst) {
                burst_credit += drained;
            }

            if (!m_worker.pop_next(next)) {
                if (m_worker.has_pending_injected()) {
                    continue;
                }

                drained = m_worker.drain_injected();
                on_remote_collected(drained);
                if (allow_injected_burst) {
                    burst_credit += drained;
                }

                if (drained == 0 || !m_worker.pop_next(next)) {
                    break;
                }
            }

            detail::invoke_ready_entry_resume(resume_fn, next);
            detail::release_ready_entry(next);
            ++summary.ran;
            if (allow_injected_burst && burst_credit > 0) {
                --burst_credit;
            }
            if (summary.ran >= m_ready_budget && (!allow_injected_burst || burst_credit == 0)) {
                break;
            }
        }

        summary.passes = 1;
        return summary;
    }

    template <typename ResumeFn, typename OnRemoteCollectedFn>
    SchedulerReadyPassSummary run_local_followup_passes(size_t max_passes,
                                                     ResumeFn&& resume_fn,
                                                     OnRemoteCollectedFn&& on_remote_collected_fn) {
        SchedulerReadyPassSummary aggregate;
        if (max_passes == 0) {
            return aggregate;
        }

        auto&& resume = resume_fn;
        auto&& on_remote_collected = on_remote_collected_fn;

        for (size_t pass = 0; pass < max_passes; ++pass) {
            auto summary = run_ready_pass_detailed(resume, on_remote_collected);
            aggregate.ran += summary.ran;
            aggregate.drainedRemote += summary.drainedRemote;
            aggregate.passes += summary.passes;

            if (summary.ran < m_ready_budget ||
                summary.drainedRemote != 0 ||
                !m_worker.has_local_work()) {
                break;
            }
        }

        return aggregate;
    }

    template <typename ResumeFn, typename OnRemoteCollectedFn>
    size_t run_ready_pass(ResumeFn&& resume_fn, OnRemoteCollectedFn&& on_remote_collected_fn) {
        return run_ready_pass_detailed(
            std::forward<ResumeFn>(resume_fn),
            std::forward<OnRemoteCollectedFn>(on_remote_collected_fn)).ran;
    }

    template <typename ResumeFn>
    size_t run_ready_pass(ResumeFn&& resume_fn) {
        return run_ready_pass(std::forward<ResumeFn>(resume_fn), [](size_t) {});
    }

    template <typename CollectCompletionsFn,
              typename PollFn,
              typename ResumeFn,
              typename StageObserverFn>
    void run_loop_iteration(CollectCompletionsFn&& collect_completions_fn,
                          PollFn&& poll_fn,
                          ResumeFn&& resume_fn,
        StageObserverFn&& stage_observer_fn) {
        stage_observer_fn(SchedulerCoreStage::CollectRemote);
        collect_remote();

        stage_observer_fn(SchedulerCoreStage::CollectCompletions);
        collect_completions_fn();

        stage_observer_fn(SchedulerCoreStage::RunReady);
        run_ready_pass(std::forward<ResumeFn>(resume_fn));

        if (!has_pending_work()) {
            if (!try_steal()) {
                stage_observer_fn(SchedulerCoreStage::Poll);
                poll_fn();
            }
        }
    }

private:
    IOReadyQueue& m_worker;
    size_t m_ready_budget;
};

}  // namespace galay::kernel

#endif  // GALAY_KERNEL_SCHEDULER_CORE_H
