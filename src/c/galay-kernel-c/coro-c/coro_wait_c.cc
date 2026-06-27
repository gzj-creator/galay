#include "coro_wait_c.h"
#include "coro_task_internal.hpp"

#include "../../../cpp/galay-kernel/common/timer.hpp"
#include "../../../cpp/galay-kernel/core/timer_scheduler.h"

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>

namespace
{

enum class WaitState : uint8_t {
    Idle,
    Pending,
    Waiting,
    Completing,
    Completed,
    Destroyed,
};

struct WaitRequestState {
    std::mutex mutex;
    uint64_t generation = 0;
    WaitState state = WaitState::Idle;
    C_IOResult result{C_IOResultInvalid, 0, 0, 0, nullptr};
    galay::kernel::coro_c::C_CoroTaskInternal* waiter = nullptr;
    galay::kernel::Timer::ptr timer;
    bool completed_waiter_pending = false;
};

using WaitRequestPtr = std::shared_ptr<WaitRequestState>;

struct WaitEventTokenState {
    WaitRequestPtr request;
    uint64_t generation = 0;
};

C_IOResult make_result(C_IOResultCode code, int sys_errno = 0)
{
    return C_IOResult{code, sys_errno, 0, 0, nullptr};
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

WaitEventTokenState* event_token(C_CoroWaitEventToken* token)
{
    return token != nullptr ? static_cast<WaitEventTokenState*>(token->token) : nullptr;
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

C_IOResult complete_state(const WaitRequestPtr& state,
                          uint64_t generation,
                          C_IOResult result,
                          bool invalidate_generation)
{
    if (!state) {
        return make_result(C_IOResultInvalid);
    }

    galay::kernel::coro_c::C_CoroTaskInternal* waiter = nullptr;
    galay::kernel::Timer::ptr timer_to_cancel;
    C_IOResult old_result{C_IOResultInvalid, 0, 0, 0, nullptr};
    uint64_t old_generation = 0;
    uint64_t committed_generation = 0;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->state == WaitState::Destroyed ||
            state->generation != generation ||
            (state->state != WaitState::Pending && state->state != WaitState::Waiting)) {
            return make_result(C_IOResultInvalid);
        }

        old_generation = state->generation;
        old_result = state->result;
        if (invalidate_generation) {
            ++state->generation;
        }
        committed_generation = state->generation;
        state->result = result;
        state->state = state->state == WaitState::Waiting
            ? WaitState::Completing
            : WaitState::Completed;
        waiter = state->waiter;
        state->completed_waiter_pending = waiter != nullptr;
        if (waiter == nullptr && state->timer) {
            timer_to_cancel = std::move(state->timer);
        }
    }

    bool resumed_waiter = true;
    if (waiter != nullptr) {
        resumed_waiter = galay::kernel::coro_c::resumeTaskFromWait(waiter);
    }
    if (waiter != nullptr && !resumed_waiter) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->state == WaitState::Completing && state->waiter == waiter) {
            state->generation = old_generation;
            state->result = old_result;
            state->state = WaitState::Waiting;
            state->completed_waiter_pending = false;
            return make_result(C_IOResultError);
        }
    }
    if (waiter != nullptr) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            const bool same_completion =
                state->generation == committed_generation &&
                (state->state == WaitState::Completing ||
                 (state->state == WaitState::Completed &&
                  state->completed_waiter_pending));
            if (same_completion && state->state == WaitState::Completing) {
                state->state = WaitState::Completed;
            }
            if (same_completion && state->waiter == waiter) {
                state->waiter = nullptr;
            }
            if (same_completion && state->timer) {
                timer_to_cancel = std::move(state->timer);
            }
        }
        galay::kernel::coro_c::releaseTask(waiter);
    }
    if (timer_to_cancel) {
        timer_to_cancel->cancel();
    }
    return make_result(result.code == C_IOResultCancelled ? C_IOResultCancelled : C_IOResultOk);
}

C_IOResult request_create_impl(C_CoroWaitRequest* out_request)
{
    if (out_request == nullptr || out_request->request != nullptr) {
        return make_result(C_IOResultInvalid);
    }

    auto state = std::make_shared<WaitRequestState>();
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

    galay::kernel::Timer::ptr timer_to_cancel;
    {
        std::lock_guard<std::mutex> lock((*state_holder)->mutex);
        if ((*state_holder)->state == WaitState::Pending ||
            (*state_holder)->state == WaitState::Waiting ||
            (*state_holder)->state == WaitState::Completing ||
            ((*state_holder)->state == WaitState::Completed &&
             (*state_holder)->completed_waiter_pending)) {
            return make_result(C_IOResultInvalid);
        }
        if ((*state_holder)->timer) {
            timer_to_cancel = std::move((*state_holder)->timer);
        }
        (*state_holder)->state = WaitState::Destroyed;
    }

    if (timer_to_cancel) {
        timer_to_cancel->cancel();
    }
    delete state_holder;
    request->request = nullptr;
    return make_result(C_IOResultOk);
}

C_IOResult request_prepare_impl(C_CoroWaitRequest* request,
                                uint64_t* out_generation)
{
    auto state = get_state(request);
    if (!state || out_generation == nullptr) {
        return make_result(C_IOResultInvalid);
    }

    galay::kernel::Timer::ptr timer_to_cancel;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->state == WaitState::Pending ||
            state->state == WaitState::Waiting ||
            state->state == WaitState::Completing ||
            state->state == WaitState::Completed ||
            state->state == WaitState::Destroyed) {
            return make_result(C_IOResultInvalid);
        }

        if (state->timer) {
            timer_to_cancel = std::move(state->timer);
        }
        ++state->generation;
        state->state = WaitState::Pending;
        state->result = make_result(C_IOResultInvalid);
        state->waiter = nullptr;
        state->completed_waiter_pending = false;
        *out_generation = state->generation;
    }
    if (timer_to_cancel) {
        timer_to_cancel->cancel();
    }
    return make_result(C_IOResultOk);
}

C_IOResult event_token_acquire_impl(C_CoroWaitRequest* request,
                                    uint64_t generation,
                                    C_CoroWaitEventToken* out_token)
{
    auto state = get_state(request);
    if (!state || out_token == nullptr || out_token->token != nullptr) {
        return make_result(C_IOResultInvalid);
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->state == WaitState::Destroyed ||
            state->generation != generation ||
            (state->state != WaitState::Pending && state->state != WaitState::Waiting)) {
            return make_result(C_IOResultInvalid);
        }
    }

    auto* token = new (std::nothrow) WaitEventTokenState{state, generation};
    if (token == nullptr) {
        return make_result(C_IOResultError);
    }
    out_token->token = token;
    return make_result(C_IOResultOk);
}

C_IOResult wait_impl(C_CoroWaitRequest* request, int64_t timeout_ms)
{
    auto state = get_state(request);
    auto* current = galay::kernel::coro_c::currentTask();
    if (!state || current == nullptr || !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }
    uint64_t timer_generation = 0;
    if (timeout_ms > 0) {
        std::lock_guard<std::mutex> lock(state->mutex);
        timer_generation = state->generation;
    }
    galay::kernel::Timer::ptr pending_timer;
    if (timeout_ms > 0) {
        std::weak_ptr<WaitRequestState> weak_state(state);
        pending_timer = std::make_shared<galay::kernel::CBTimer>(
            std::chrono::milliseconds(timeout_ms),
            [weak_state, generation = timer_generation]() {
                if (auto locked = weak_state.lock()) {
                    (void)complete_state(locked,
                                         generation,
                                         make_result(C_IOResultTimeout),
                                         true);
                }
            });
    }
    if (!galay::kernel::coro_c::prepareCurrentTaskWait()) {
        return make_result(C_IOResultInvalid);
    }

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->state == WaitState::Destroyed) {
            (void)galay::kernel::coro_c::rollbackCurrentTaskWait();
            return make_result(C_IOResultInvalid);
        }
        if (state->state == WaitState::Completed) {
            C_IOResult result = state->result;
            state->state = WaitState::Idle;
            state->completed_waiter_pending = false;
            (void)galay::kernel::coro_c::rollbackCurrentTaskWait();
            return result;
        }
        if (state->state != WaitState::Pending) {
            (void)galay::kernel::coro_c::rollbackCurrentTaskWait();
            return make_result(C_IOResultInvalid);
        }
        if (timeout_ms == 0) {
            ++state->generation;
            state->state = WaitState::Idle;
            state->result = make_result(C_IOResultTimeout);
            state->completed_waiter_pending = false;
            (void)galay::kernel::coro_c::rollbackCurrentTaskWait();
            return state->result;
        }

        state->state = WaitState::Waiting;
        state->waiter = current;
        galay::kernel::coro_c::retainTask(current);
        if (pending_timer) {
            if (!galay::kernel::TimerScheduler::getInstance()->addTimer(pending_timer)) {
                state->waiter = nullptr;
                state->state = WaitState::Pending;
                galay::kernel::coro_c::releaseTask(current);
                (void)galay::kernel::coro_c::rollbackCurrentTaskWait();
                return make_result(C_IOResultError);
            }
            state->timer = std::move(pending_timer);
        }
    }

    C_IOResult parked = galay::kernel::coro_c::parkPreparedCurrentTaskWait();
    if (parked.code != C_IOResultOk) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->waiter == current) {
            state->waiter = nullptr;
            state->state = WaitState::Pending;
            galay::kernel::coro_c::releaseTask(current);
        }
        (void)galay::kernel::coro_c::rollbackCurrentTaskWait();
        return parked;
    }

    galay::kernel::Timer::ptr timer_to_cancel;
    C_IOResult result;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        result = state->result;
        state->state = WaitState::Idle;
        state->completed_waiter_pending = false;
        if (state->waiter == current) {
            state->waiter = nullptr;
        }
        if (state->timer) {
            timer_to_cancel = std::move(state->timer);
        }
    }
    if (timer_to_cancel) {
        timer_to_cancel->cancel();
    }
    return result;
}

C_IOResult event_token_release_impl(C_CoroWaitEventToken* token)
{
    auto* token_state = event_token(token);
    if (token_state == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    token->token = nullptr;
    delete token_state;
    return make_result(C_IOResultOk);
}

C_IOResult event_token_detach_user_data_impl(C_CoroWaitEventToken* token,
                                             void** out_user_data)
{
    auto* token_state = event_token(token);
    if (token_state == nullptr || out_user_data == nullptr || *out_user_data != nullptr) {
        return make_result(C_IOResultInvalid);
    }
    token->token = nullptr;
    *out_user_data = token_state;
    return make_result(C_IOResultOk);
}

C_IOResult event_token_complete_impl(C_CoroWaitEventToken* token,
                                     C_IOResult result)
{
    auto* token_state = event_token(token);
    if (token_state == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    return complete_state(token_state->request, token_state->generation, result, false);
}

C_IOResult event_token_cancel_impl(C_CoroWaitEventToken* token)
{
    auto* token_state = event_token(token);
    if (token_state == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    return complete_state(token_state->request,
                          token_state->generation,
                          make_result(C_IOResultCancelled),
                          true);
}

C_IOResult event_user_data_complete_impl(void* user_data,
                                         C_IOResult result)
{
    auto* token_state = user_data_token(user_data);
    if (token_state == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    return complete_state(token_state->request, token_state->generation, result, false);
}

C_IOResult event_user_data_cancel_impl(void* user_data)
{
    auto* token_state = user_data_token(user_data);
    if (token_state == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    return complete_state(token_state->request,
                          token_state->generation,
                          make_result(C_IOResultCancelled),
                          true);
}

C_IOResult event_user_data_release_impl(void* user_data)
{
    auto* token_state = user_data_token(user_data);
    if (token_state == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    delete token_state;
    return make_result(C_IOResultOk);
}

} // namespace

extern "C" {

C_IOResult galay_coro_wait_request_create(C_CoroWaitRequest* out_request)
{
    try {
        return request_create_impl(out_request);
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_wait_request_destroy(C_CoroWaitRequest* request)
{
    try {
        return request_destroy_impl(request);
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_wait_request_prepare(C_CoroWaitRequest* request,
                                           uint64_t* out_generation)
{
    try {
        return request_prepare_impl(request, out_generation);
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_wait_request_event_token_acquire(C_CoroWaitRequest* request,
                                                       uint64_t generation,
                                                       C_CoroWaitEventToken* out_token)
{
    try {
        return event_token_acquire_impl(request, generation, out_token);
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_wait(C_CoroWaitRequest* request, int64_t timeout_ms)
{
    try {
        return wait_impl(request, timeout_ms);
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_wait_event_token_detach_user_data(C_CoroWaitEventToken* token,
                                                        void** out_user_data)
{
    try {
        return event_token_detach_user_data_impl(token, out_user_data);
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_wait_request_complete(C_CoroWaitRequest* request,
                                            uint64_t generation,
                                            C_IOResult result)
{
    try {
        auto state = get_state(request);
        return complete_state(state, generation, result, false);
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_wait_request_cancel(C_CoroWaitRequest* request,
                                          uint64_t generation)
{
    try {
        auto state = get_state(request);
        return complete_state(state, generation, make_result(C_IOResultCancelled), true);
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_wait_event_token_complete(C_CoroWaitEventToken* token,
                                                C_IOResult result)
{
    try {
        return event_token_complete_impl(token, result);
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_wait_event_token_cancel(C_CoroWaitEventToken* token)
{
    try {
        return event_token_cancel_impl(token);
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_wait_event_token_release(C_CoroWaitEventToken* token)
{
    try {
        return event_token_release_impl(token);
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_wait_event_user_data_complete(void* user_data,
                                                    C_IOResult result)
{
    try {
        return event_user_data_complete_impl(user_data, result);
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_wait_event_user_data_cancel(void* user_data)
{
    try {
        return event_user_data_cancel_impl(user_data);
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_wait_event_user_data_release(void* user_data)
{
    try {
        return event_user_data_release_impl(user_data);
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

} // extern "C"
