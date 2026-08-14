#include "coro_wait_c.h"
#include "coro_task_internal.hpp"

#include "../../../cpp/galay-kernel/common/timer.hpp"
#include "../../../cpp/galay-kernel/core/timer_scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <utility>

namespace
{

enum class WaitState : uint8_t {
    Idle,
    Preparing,
    Pending,
    Waiting,
    Completing,
    Completed,
    Destroyed,
};

struct WaitRequestState;
using WaitRequestPtr = std::shared_ptr<WaitRequestState>;

/**
 * @brief backend 持有的事件 token。
 * @details token 仍内嵌在 request 中；owner shared_ptr 在 token 存活期间形成
 *          短暂自引用，因而晚到的 backend completion 不会解引用已销毁的 request
 *          句柄。completion 线程只读取原子 raw pointer，避免标准库原子智能指针
 *          操作在 libstdc++ 中使用的隐藏锁。
 */
struct WaitEventTokenState {
    std::atomic<WaitRequestState*> request{nullptr};
    WaitRequestPtr owner{};
    std::atomic<uint64_t> generation{0};
    std::atomic_bool in_use{false};
};

struct WaitRequestState {
    // request 状态只通过 CAS 推进，完成路径不获取阻塞锁。
    std::atomic<WaitState> state{WaitState::Idle};
    std::atomic<uint64_t> generation{0};
    C_IOResult result{C_IOResultInvalid, 0, 0, 0, nullptr};
    std::atomic<galay::kernel::coro_c::C_CoroTaskInternal*> waiter{nullptr};
    std::atomic_bool waiter_pending{false};

    // timer 只由 owner coroutine 创建、消费和取消；completion 线程不访问该字段。
    galay::kernel::Timer::ptr timer;

    std::atomic_bool fast_wait_active{false};
    std::atomic_bool fast_wait_completed{false};
    std::atomic<galay::kernel::coro_c::C_CoroTaskInternal*> fast_waiter{nullptr};
    C_IOResult fast_wait_result{C_IOResultInvalid, 0, 0, 0, nullptr};
    WaitEventTokenState token_slot;
};

C_IOResult make_result(C_IOResultCode code, int sys_errno = 0)
{
    return C_IOResult{code, sys_errno, 0, 0, nullptr};
}

inline void wait_state_relax() noexcept
{
#if defined(__x86_64__) || defined(__i386__)
    __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    std::this_thread::yield();
#endif
}

C_IOResult merge_cleanup_result(C_IOResult primary, C_IOResult cleanup)
{
    return primary.code == C_IOResultOk && cleanup.code != C_IOResultOk
        ? cleanup
        : primary;
}

WaitRequestPtr* holder(C_CoroWaitRequest* request)
{
    return request != nullptr ? static_cast<WaitRequestPtr*>(request->request) : nullptr;
}

WaitRequestPtr get_state(C_CoroWaitRequest* request)
{
    auto* state_holder = holder(request);
    return state_holder != nullptr ? *state_holder : WaitRequestPtr{};
}

WaitEventTokenState* user_data_token(void* user_data)
{
    return static_cast<WaitEventTokenState*>(user_data);
}

bool timeout_fits_chrono(int64_t timeout_ms)
{
    if (timeout_ms <= 0) {
        return true;
    }
    using MillisecondsRep = std::chrono::milliseconds::rep;
    using NanosecondsRep = std::chrono::nanoseconds::rep;
    constexpr auto max_milliseconds_rep =
        static_cast<int64_t>(std::numeric_limits<MillisecondsRep>::max());
    constexpr auto max_milliseconds_for_nanoseconds =
        static_cast<int64_t>(std::numeric_limits<NanosecondsRep>::max() / 1000000);
    constexpr int64_t max_supported_milliseconds =
        max_milliseconds_rep < max_milliseconds_for_nanoseconds
            ? max_milliseconds_rep
            : max_milliseconds_for_nanoseconds;
    return timeout_ms <= max_supported_milliseconds;
}

void cancel_timer(WaitRequestState& state) noexcept
{
    auto timer = std::move(state.timer);
    if (timer) {
        timer->cancel();
    }
}

void release_waiter_reference(WaitRequestState& state,
                              galay::kernel::coro_c::C_CoroTaskInternal* current) noexcept
{
    auto* claimed = state.waiter.exchange(nullptr, std::memory_order_acq_rel);
    if (claimed == current) {
        galay::kernel::coro_c::releaseTask(current);
    }
    state.waiter_pending.store(false, std::memory_order_release);
}

/**
 * @brief Consume a completed request from the owner coroutine.
 * @details result is read before the Completed->Idle CAS so a new generation cannot
 *          overwrite the plain result object before it is copied.
 */
C_IOResult take_completed_result(const WaitRequestPtr& state,
                                 galay::kernel::coro_c::C_CoroTaskInternal* current)
{
    if (!state || state->state.load(std::memory_order_acquire) != WaitState::Completed) {
        return make_result(C_IOResultInvalid);
    }

    C_IOResult result = state->result;
    cancel_timer(*state);
    release_waiter_reference(*state, current);
    WaitState expected = WaitState::Completed;
    if (!state->state.compare_exchange_strong(expected,
                                              WaitState::Idle,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        return make_result(C_IOResultInvalid);
    }
    // A completion can win before parkPreparedCurrentTaskWait. In that case the
    // ready queue entry is intentionally left as a no-op and this task resumes here.
    (void)galay::kernel::coro_c::activatePreparedCurrentTaskWait();
    return result;
}

// Forward declaration used by timer callbacks without exposing the internal state type.
C_IOResult complete_state(WaitRequestState* state,
                          uint64_t generation,
                          C_IOResult result,
                          bool invalidate_generation);

galay::kernel::Timer::ptr create_wait_timer_impl(int64_t timeout_ms,
                                                 const WaitRequestPtr& state,
                                                 uint64_t generation,
                                                 C_IOResult result)
{
    std::weak_ptr<WaitRequestState> weak_state(state);
    auto timer = std::unique_ptr<galay::kernel::CBTimer>(new (std::nothrow) galay::kernel::CBTimer(
        std::chrono::milliseconds(timeout_ms),
        [weak_state, generation, result]() {
            if (auto locked = weak_state.lock()) {
                (void)complete_state(locked.get(), generation, result, true);
            }
        }));
    if (!timer) {
        return {};
    }
    return galay::kernel::Timer::ptr(std::move(timer));
}

C_IOResult complete_state(WaitRequestState* state,
                          uint64_t generation,
                          C_IOResult result,
                          bool invalidate_generation)
{
    if (!state || state->generation.load(std::memory_order_acquire) != generation) {
        return make_result(C_IOResultInvalid);
    }

    WaitState phase = state->state.load(std::memory_order_acquire);
    for (;;) {
        if (phase != WaitState::Pending && phase != WaitState::Waiting) {
            return make_result(C_IOResultInvalid);
        }
        if (state->generation.load(std::memory_order_acquire) != generation) {
            return make_result(C_IOResultInvalid);
        }

        WaitState expected = phase;
        if (!state->state.compare_exchange_weak(expected,
                                                WaitState::Completing,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            phase = expected;
            continue;
        }

        const C_IOResult old_result = state->result;
        state->result = result;
        if (invalidate_generation) {
            state->generation.fetch_add(1, std::memory_order_acq_rel);
        }

        auto* waiter = state->waiter.exchange(nullptr, std::memory_order_acq_rel);
        const bool direct_resume = waiter != nullptr &&
            galay::kernel::coro_c::canResumeTaskFromWaitImmediately(waiter);
        if (direct_resume || waiter == nullptr) {
            state->state.store(WaitState::Completed, std::memory_order_release);
        }

        bool resumed_waiter = true;
        if (waiter != nullptr) {
            resumed_waiter = direct_resume
                ? galay::kernel::coro_c::resumeTaskFromWaitImmediately(waiter)
                : galay::kernel::coro_c::resumeTaskFromWait(waiter);
            if (resumed_waiter && !direct_resume) {
                state->state.store(WaitState::Completed, std::memory_order_release);
            }
        }

        if (!resumed_waiter) {
            // Queue admission failure is a recoverable completion error. Restore the
            // request so a caller can retry while the parked task remains Waiting.
            state->result = old_result;
            if (invalidate_generation) {
                state->generation.store(generation, std::memory_order_release);
            }
            state->waiter.store(waiter, std::memory_order_release);
            state->waiter_pending.store(waiter != nullptr, std::memory_order_release);
            state->state.store(phase, std::memory_order_release);
            return make_result(C_IOResultError);
        }

        if (waiter != nullptr) {
            galay::kernel::coro_c::releaseTask(waiter);
        }
        return make_result(result.code == C_IOResultCancelled
                               ? C_IOResultCancelled
                               : C_IOResultOk);
    }
}

C_IOResult request_create_impl(C_CoroWaitRequest* out_request)
{
    if (out_request == nullptr || out_request->request != nullptr) {
        return make_result(C_IOResultInvalid);
    }

    auto raw_state = std::unique_ptr<WaitRequestState>(new (std::nothrow) WaitRequestState());
    if (!raw_state) {
        return make_result(C_IOResultError);
    }
    WaitRequestPtr state(std::move(raw_state));
    auto* state_holder = new (std::nothrow) WaitRequestPtr(std::move(state));
    if (state_holder == nullptr || !*state_holder) {
        delete state_holder;
        return make_result(C_IOResultError);
    }
    out_request->request = state_holder;
    return make_result(C_IOResultOk);
}

C_IOResult request_destroy_impl(C_CoroWaitRequest* request)
{
    auto* state_holder = holder(request);
    if (state_holder == nullptr || !*state_holder) {
        return make_result(C_IOResultInvalid);
    }
    auto& state = **state_holder;
    if (state.waiter_pending.load(std::memory_order_acquire) ||
        state.fast_wait_active.load(std::memory_order_acquire)) {
        return make_result(C_IOResultInvalid);
    }
    WaitState expected = state.state.load(std::memory_order_acquire);
    if (expected != WaitState::Idle && expected != WaitState::Completed) {
        return make_result(C_IOResultInvalid);
    }
    if (!state.state.compare_exchange_strong(expected,
                                             WaitState::Destroyed,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
        return make_result(C_IOResultInvalid);
    }
    cancel_timer(state);
    delete state_holder;
    request->request = nullptr;
    return make_result(C_IOResultOk);
}

C_IOResult request_prepare_impl(C_CoroWaitRequest* request,
                                uint64_t* out_generation)
{
    auto state = get_state(request);
    if (!state || out_generation == nullptr ||
        state->token_slot.in_use.load(std::memory_order_acquire)) {
        return make_result(C_IOResultInvalid);
    }

    WaitState expected = WaitState::Idle;
    if (!state->state.compare_exchange_strong(expected,
                                              WaitState::Preparing,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        return make_result(C_IOResultInvalid);
    }

    const uint64_t generation =
        state->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
    state->result = make_result(C_IOResultInvalid);
    state->waiter.store(nullptr, std::memory_order_relaxed);
    state->waiter_pending.store(false, std::memory_order_relaxed);
    cancel_timer(*state);
    *out_generation = generation;
    state->state.store(WaitState::Pending, std::memory_order_release);
    return make_result(C_IOResultOk);
}

C_IOResult prepare_fast_wait_user_data_impl(C_CoroWaitRequest* request, void** out_user_data)
{
    auto* state_holder = holder(request);
    if (state_holder == nullptr || !*state_holder || out_user_data == nullptr ||
        *out_user_data != nullptr) {
        return make_result(C_IOResultInvalid);
    }
    auto& state = **state_holder;
    bool expected = false;
    if (!state.fast_wait_active.compare_exchange_strong(expected,
                                                        true,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
        return make_result(C_IOResultInvalid);
    }
    state.fast_wait_result = make_result(C_IOResultInvalid);
    state.fast_waiter.store(nullptr, std::memory_order_relaxed);
    state.fast_wait_completed.store(false, std::memory_order_release);
    *out_user_data = state_holder;
    return make_result(C_IOResultOk);
}

C_IOResult wait_fast_impl(C_CoroWaitRequest* request, int64_t timeout_ms)
{
    auto* state_holder = holder(request);
    if (state_holder == nullptr || !*state_holder || timeout_ms >= 0) {
        return make_result(C_IOResultInvalid);
    }
    auto& state = **state_holder;
    if (!state.fast_wait_active.load(std::memory_order_acquire) &&
        !state.fast_wait_completed.load(std::memory_order_acquire)) {
        return make_result(C_IOResultInvalid);
    }
    auto* current = galay::kernel::coro_c::currentTask();
    if (current == nullptr || !galay::kernel::coro_c::prepareCurrentTaskWait()) {
        return make_result(C_IOResultInvalid);
    }
    galay::kernel::coro_c::retainTask(current);

    if (!state.fast_wait_completed.load(std::memory_order_acquire)) {
        auto* expected = static_cast<galay::kernel::coro_c::C_CoroTaskInternal*>(nullptr);
        if (!state.fast_waiter.compare_exchange_strong(expected,
                                                        current,
                                                        std::memory_order_acq_rel,
                                                        std::memory_order_acquire)) {
            if (!state.fast_wait_completed.load(std::memory_order_acquire)) {
                galay::kernel::coro_c::releaseTask(current);
                (void)galay::kernel::coro_c::rollbackCurrentTaskWait();
                return make_result(C_IOResultInvalid);
            }
        }
    }

    if (state.fast_wait_completed.load(std::memory_order_acquire)) {
        auto* claimed = state.fast_waiter.exchange(nullptr, std::memory_order_acq_rel);
        if (claimed == current) {
            (void)galay::kernel::coro_c::rollbackCurrentTaskWait();
            galay::kernel::coro_c::releaseTask(current);
            return state.fast_wait_result;
        }
        if (claimed != nullptr) {
            galay::kernel::coro_c::releaseTask(current);
            (void)galay::kernel::coro_c::rollbackCurrentTaskWait();
            return make_result(C_IOResultInvalid);
        }
        if (galay::kernel::coro_c::activatePreparedCurrentTaskWait()) {
            galay::kernel::coro_c::releaseTask(current);
            return state.fast_wait_result;
        }
    }

    C_IOResult parked = galay::kernel::coro_c::parkPreparedCurrentTaskWait();
    if (parked.code != C_IOResultOk) {
        if (state.fast_waiter.exchange(nullptr, std::memory_order_acq_rel) == current) {
            galay::kernel::coro_c::releaseTask(current);
        }
        (void)galay::kernel::coro_c::rollbackCurrentTaskWait();
        return parked;
    }
    if (!state.fast_wait_completed.load(std::memory_order_acquire)) {
        return make_result(C_IOResultError);
    }
    return state.fast_wait_result;
}

C_IOResult complete_fast_wait_user_data_impl(void* user_data, C_IOResult result)
{
    auto* state_holder = static_cast<WaitRequestPtr*>(user_data);
    if (state_holder == nullptr || !*state_holder ||
        !(*state_holder)->fast_wait_active.load(std::memory_order_acquire)) {
        return make_result(C_IOResultInvalid);
    }
    auto& state = **state_holder;
    state.fast_wait_result = result;
    state.fast_wait_completed.store(true, std::memory_order_release);
    auto* waiter = state.fast_waiter.exchange(nullptr, std::memory_order_acq_rel);
    if (waiter == nullptr) {
        return make_result(C_IOResultOk);
    }
    bool resumed = galay::kernel::coro_c::resumeTaskFromWaitImmediately(waiter);
    if (!resumed) {
        resumed = galay::kernel::coro_c::resumeTaskFromWait(waiter);
    }
    galay::kernel::coro_c::releaseTask(waiter);
    return make_result(resumed ? C_IOResultOk : C_IOResultError);
}

C_IOResult release_fast_wait_user_data_impl(void* user_data)
{
    auto* state_holder = static_cast<WaitRequestPtr*>(user_data);
    if (state_holder == nullptr || !*state_holder ||
        !(*state_holder)->fast_wait_active.exchange(false, std::memory_order_acq_rel)) {
        return make_result(C_IOResultInvalid);
    }
    return make_result(C_IOResultOk);
}

WaitEventTokenState* token_slot_of(C_CoroWaitEventToken* token)
{
    return token != nullptr ? static_cast<WaitEventTokenState*>(token->token) : nullptr;
}

WaitRequestState* token_request(WaitEventTokenState* slot)
{
    return slot != nullptr
        ? slot->request.load(std::memory_order_acquire)
        : nullptr;
}

C_IOResult event_token_acquire_impl(C_CoroWaitRequest* request,
                                    uint64_t generation,
                                    C_CoroWaitEventToken* out_token)
{
    auto state = get_state(request);
    if (!state || out_token == nullptr || out_token->token != nullptr) {
        return make_result(C_IOResultInvalid);
    }
    auto& slot = state->token_slot;
    if (slot.in_use.load(std::memory_order_acquire) ||
        state->generation.load(std::memory_order_acquire) != generation) {
        return make_result(C_IOResultInvalid);
    }
    const WaitState phase = state->state.load(std::memory_order_acquire);
    if (phase != WaitState::Pending && phase != WaitState::Waiting) {
        return make_result(C_IOResultInvalid);
    }
    slot.owner = state;
    slot.request.store(state.get(), std::memory_order_release);
    slot.generation.store(generation, std::memory_order_release);
    slot.in_use.store(true, std::memory_order_release);
    out_token->token = &slot;
    return make_result(C_IOResultOk);
}

C_IOResult wait_impl(C_CoroWaitRequest* request,
                     int64_t timeout_ms,
                     C_IOResult timeout_result = make_result(C_IOResultTimeout))
{
    auto state = get_state(request);
    auto* current = galay::kernel::coro_c::currentTask();
    if (!state || current == nullptr || !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }

    const uint64_t timer_generation = state->generation.load(std::memory_order_acquire);
    if (timeout_ms == 0) {
        (void)complete_state(state.get(), timer_generation, timeout_result, true);
        if (state->state.load(std::memory_order_acquire) == WaitState::Completed) {
            return take_completed_result(state, nullptr);
        }
        return make_result(C_IOResultInvalid);
    }
    if (state->state.load(std::memory_order_acquire) == WaitState::Completed) {
        return take_completed_result(state, current);
    }
    if (state->state.load(std::memory_order_acquire) != WaitState::Pending) {
        return make_result(C_IOResultInvalid);
    }

    galay::kernel::Timer::ptr pending_timer;
    if (timeout_ms > 0) {
        pending_timer = create_wait_timer_impl(timeout_ms,
                                               state,
                                               timer_generation,
                                               timeout_result);
        if (!pending_timer) {
            return make_result(C_IOResultError);
        }
    }
    if (!galay::kernel::coro_c::prepareCurrentTaskWait()) {
        return make_result(C_IOResultInvalid);
    }

    if (state->state.load(std::memory_order_acquire) == WaitState::Completed) {
        return take_completed_result(state, current);
    }
    if (state->state.load(std::memory_order_acquire) != WaitState::Pending) {
        (void)galay::kernel::coro_c::rollbackCurrentTaskWait();
        return make_result(C_IOResultInvalid);
    }

    galay::kernel::coro_c::retainTask(current);
    state->waiter.store(current, std::memory_order_release);
    state->waiter_pending.store(true, std::memory_order_release);
    WaitState expected = WaitState::Pending;
    if (!state->state.compare_exchange_strong(expected,
                                              WaitState::Waiting,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
        while (expected == WaitState::Completing) {
            expected = state->state.load(std::memory_order_acquire);
            wait_state_relax();
        }
        if (state->state.load(std::memory_order_acquire) == WaitState::Completed) {
            return take_completed_result(state, current);
        }
        release_waiter_reference(*state, current);
        (void)galay::kernel::coro_c::rollbackCurrentTaskWait();
        return make_result(C_IOResultInvalid);
    }

    if (pending_timer) {
        state->timer = std::move(pending_timer);
        if (!galay::kernel::TimerScheduler::getInstance()->addTimer(state->timer)) {
            cancel_timer(*state);
            (void)complete_state(state.get(),
                                 timer_generation,
                                 make_result(C_IOResultError),
                                 true);
            while (state->state.load(std::memory_order_acquire) == WaitState::Completing) {
                wait_state_relax();
            }
            if (state->state.load(std::memory_order_acquire) == WaitState::Completed) {
                return take_completed_result(state, current);
            }
            release_waiter_reference(*state, current);
            (void)galay::kernel::coro_c::rollbackCurrentTaskWait();
            return make_result(C_IOResultError);
        }
    }

    C_IOResult parked = galay::kernel::coro_c::parkPreparedCurrentTaskWait();
    if (parked.code != C_IOResultOk) {
        WaitState waiting = WaitState::Waiting;
        if (state->state.compare_exchange_strong(waiting,
                                                 WaitState::Pending,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire)) {
            cancel_timer(*state);
            release_waiter_reference(*state, current);
            (void)galay::kernel::coro_c::rollbackCurrentTaskWait();
            return parked;
        }
        while (state->state.load(std::memory_order_acquire) == WaitState::Completing) {
            wait_state_relax();
        }
    }

    while (state->state.load(std::memory_order_acquire) == WaitState::Completing) {
        wait_state_relax();
    }
    if (state->state.load(std::memory_order_acquire) != WaitState::Completed) {
        return make_result(C_IOResultError);
    }
    return take_completed_result(state, current);
}

C_IOResult event_token_release_impl(C_CoroWaitEventToken* token)
{
    auto* slot = token_slot_of(token);
    if (slot == nullptr || !slot->in_use.exchange(false, std::memory_order_acq_rel)) {
        return make_result(C_IOResultInvalid);
    }
    token->token = nullptr;
    slot->request.store(nullptr, std::memory_order_release);
    WaitRequestPtr owner = std::move(slot->owner);
    owner.reset();
    return make_result(C_IOResultOk);
}

C_IOResult event_token_detach_user_data_impl(C_CoroWaitEventToken* token,
                                             void** out_user_data)
{
    auto* slot = token_slot_of(token);
    if (slot == nullptr || out_user_data == nullptr || *out_user_data != nullptr ||
        !slot->in_use.load(std::memory_order_acquire)) {
        return make_result(C_IOResultInvalid);
    }
    token->token = nullptr;
    *out_user_data = slot;
    return make_result(C_IOResultOk);
}

C_IOResult event_token_complete_impl(C_CoroWaitEventToken* token, C_IOResult result)
{
    auto* slot = token_slot_of(token);
    auto state = token_request(slot);
    if (slot == nullptr || !slot->in_use.load(std::memory_order_acquire) || !state) {
        return make_result(C_IOResultInvalid);
    }
    return complete_state(state, slot->generation.load(std::memory_order_acquire), result, false);
}

C_IOResult event_token_cancel_impl(C_CoroWaitEventToken* token)
{
    auto* slot = token_slot_of(token);
    auto state = token_request(slot);
    if (slot == nullptr || !slot->in_use.load(std::memory_order_acquire) || !state) {
        return make_result(C_IOResultInvalid);
    }
    return complete_state(state,
                          slot->generation.load(std::memory_order_acquire),
                          make_result(C_IOResultCancelled),
                          true);
}

C_IOResult event_user_data_complete_impl(void* user_data, C_IOResult result)
{
    auto* slot = user_data_token(user_data);
    auto state = token_request(slot);
    if (slot == nullptr || !slot->in_use.load(std::memory_order_acquire) || !state) {
        return make_result(C_IOResultInvalid);
    }
    return complete_state(state, slot->generation.load(std::memory_order_acquire), result, false);
}

C_IOResult event_user_data_cancel_impl(void* user_data)
{
    auto* slot = user_data_token(user_data);
    auto state = token_request(slot);
    if (slot == nullptr || !slot->in_use.load(std::memory_order_acquire) || !state) {
        return make_result(C_IOResultInvalid);
    }
    return complete_state(state,
                          slot->generation.load(std::memory_order_acquire),
                          make_result(C_IOResultCancelled),
                          true);
}

C_IOResult event_user_data_release_impl(void* user_data)
{
    auto* slot = user_data_token(user_data);
    if (slot == nullptr || !slot->in_use.exchange(false, std::memory_order_acq_rel)) {
        return make_result(C_IOResultInvalid);
    }
    slot->request.store(nullptr, std::memory_order_release);
    WaitRequestPtr owner = std::move(slot->owner);
    owner.reset();
    return make_result(C_IOResultOk);
}

C_IOResult sleep_impl(int64_t timeout_ms)
{
    if (timeout_ms < 0 || !timeout_fits_chrono(timeout_ms) ||
        galay::kernel::coro_c::currentTask() == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultOk);
    }

    C_CoroWaitRequest request{nullptr};
    C_IOResult created = request_create_impl(&request);
    if (created.code != C_IOResultOk) {
        return created;
    }
    uint64_t generation = 0;
    C_IOResult prepared = request_prepare_impl(&request, &generation);
    if (prepared.code != C_IOResultOk) {
        C_IOResult destroyed = request_destroy_impl(&request);
        return merge_cleanup_result(prepared, destroyed);
    }
    C_IOResult waited = wait_impl(&request,
                                  timeout_ms,
                                  make_result(C_IOResultOk));
    C_IOResult destroyed = request_destroy_impl(&request);
    return merge_cleanup_result(waited, destroyed);
}

} // namespace

namespace galay::kernel::coro_c
{

C_IOResult prepareFastWaitUserData(C_CoroWaitRequest* request,
                                   void** out_user_data) noexcept
{
    return prepare_fast_wait_user_data_impl(request, out_user_data);
}

C_IOResult waitFastRequest(C_CoroWaitRequest* request, int64_t timeout_ms) noexcept
{
    return wait_fast_impl(request, timeout_ms);
}

C_IOResult completeFastWaitUserData(void* user_data, C_IOResult result) noexcept
{
    return complete_fast_wait_user_data_impl(user_data, result);
}

C_IOResult releaseFastWaitUserData(void* user_data) noexcept
{
    return release_fast_wait_user_data_impl(user_data);
}

} // namespace galay::kernel::coro_c

extern "C" {

C_IOResult galay_coro_wait_request_create(C_CoroWaitRequest* out_request)
{
    return request_create_impl(out_request);
}

C_IOResult galay_coro_wait_request_destroy(C_CoroWaitRequest* request)
{
    return request_destroy_impl(request);
}

C_IOResult galay_coro_wait_request_prepare(C_CoroWaitRequest* request,
                                           uint64_t* out_generation)
{
    return request_prepare_impl(request, out_generation);
}

C_IOResult galay_coro_wait_request_event_token_acquire(C_CoroWaitRequest* request,
                                                       uint64_t generation,
                                                       C_CoroWaitEventToken* out_token)
{
    return event_token_acquire_impl(request, generation, out_token);
}

C_IOResult galay_coro_wait(C_CoroWaitRequest* request, int64_t timeout_ms)
{
    return wait_impl(request, timeout_ms);
}

C_IOResult galay_coro_wait_event_token_detach_user_data(C_CoroWaitEventToken* token,
                                                        void** out_user_data)
{
    return event_token_detach_user_data_impl(token, out_user_data);
}

C_IOResult galay_coro_wait_request_complete(C_CoroWaitRequest* request,
                                            uint64_t generation,
                                            C_IOResult result)
{
    auto state = get_state(request);
    return complete_state(state.get(), generation, result, false);
}

C_IOResult galay_coro_wait_request_cancel(C_CoroWaitRequest* request,
                                          uint64_t generation)
{
    auto state = get_state(request);
    return complete_state(state.get(),
                          generation,
                          make_result(C_IOResultCancelled),
                          true);
}

C_IOResult galay_coro_wait_event_token_complete(C_CoroWaitEventToken* token,
                                                C_IOResult result)
{
    return event_token_complete_impl(token, result);
}

C_IOResult galay_coro_wait_event_token_cancel(C_CoroWaitEventToken* token)
{
    return event_token_cancel_impl(token);
}

C_IOResult galay_coro_wait_event_token_release(C_CoroWaitEventToken* token)
{
    return event_token_release_impl(token);
}

C_IOResult galay_coro_wait_event_user_data_complete(void* user_data,
                                                    C_IOResult result)
{
    return event_user_data_complete_impl(user_data, result);
}

C_IOResult galay_coro_wait_event_user_data_cancel(void* user_data)
{
    return event_user_data_cancel_impl(user_data);
}

C_IOResult galay_coro_wait_event_user_data_release(void* user_data)
{
    return event_user_data_release_impl(user_data);
}

C_IOResult galay_coro_sleep(int64_t timeout_ms)
{
    return sleep_impl(timeout_ms);
}

} // extern "C"
