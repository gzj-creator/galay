#include "coro_task_c.h"
#include "coro_task_internal.hpp"

#include "../../../cpp/galay-kernel/core/runtime.h"
#include "../../../cpp/galay-kernel/core/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <new>
#include <thread>

#include <sys/mman.h>
#include <unistd.h>

namespace
{

#if defined(__APPLE__) && defined(__aarch64__)
#define GALAY_C_CORO_HAS_CONTEXT 1
#else
#define GALAY_C_CORO_HAS_CONTEXT 0
#endif

constexpr size_t kDefaultStackSize = 64 * 1024;
constexpr size_t kMinStackSize = 16 * 1024;

struct C_CoroResumeTokenState {
    galay::kernel::detail::ResumeTokenHeader header;
    galay::kernel::coro_c::C_CoroTaskInternal* task = nullptr;
};

struct C_CoroMachineContext {
    uint64_t x19 = 0;
    uint64_t x20 = 0;
    uint64_t x21 = 0;
    uint64_t x22 = 0;
    uint64_t x23 = 0;
    uint64_t x24 = 0;
    uint64_t x25 = 0;
    uint64_t x26 = 0;
    uint64_t x27 = 0;
    uint64_t x28 = 0;
    uint64_t x29 = 0;
    uint64_t pc = 0;
    uint64_t sp = 0;
    uint64_t d8 = 0;
    uint64_t d9 = 0;
    uint64_t d10 = 0;
    uint64_t d11 = 0;
    uint64_t d12 = 0;
    uint64_t d13 = 0;
    uint64_t d14 = 0;
    uint64_t d15 = 0;
};

static_assert(sizeof(C_CoroMachineContext) == 168);

extern "C" void galay_coro_context_switch(C_CoroMachineContext* from,
                                           C_CoroMachineContext* to);
extern "C" void galay_coro_context_entry(void);

enum class C_CoroState : uint8_t {
    Ready,
    Running,
    Waiting,
    WaitReady,
    Done,
    Cancelled,
};

} // namespace

namespace galay::kernel::coro_c
{

struct C_CoroTaskInternal {
    galay::kernel::detail::ReadyEntryCoroHeader ready_header;
    C_CoroResumeTokenState resume_token;
    std::atomic<uint32_t> ref_count{1};
    std::atomic<C_CoroState> state{C_CoroState::Ready};
    std::atomic<bool> queued{false};
    galay::kernel::Scheduler* owner = nullptr;
    std::thread::id owner_thread;
    galay_coro_entry_fn entry = nullptr;
    void* arg = nullptr;
    C_CoroMachineContext scheduler_context;
    C_CoroMachineContext context;
    void* stack_mapping = nullptr;
    size_t stack_mapping_size = 0;
    void* stack_usable = nullptr;
    size_t stack_usable_size = 0;
    std::mutex mutex;
    std::condition_variable cv;
    C_IOResult result{C_IOResultOk, 0, 0, 0, nullptr};
};

} // namespace galay::kernel::coro_c

namespace
{

using galay::kernel::coro_c::C_CoroTaskInternal;

static_assert(offsetof(C_CoroTaskInternal, ready_header) == 0);

thread_local C_CoroTaskInternal* t_current_task = nullptr;

C_IOResult make_result(C_IOResultCode code, int sys_errno = 0)
{
    return C_IOResult{code, sys_errno, 0, 0, nullptr};
}

galay::kernel::Runtime* to_cpp_runtime(galay_kernel_runtime_t* runtime)
{
    return runtime != nullptr
        ? static_cast<galay::kernel::Runtime*>(runtime->runtime)
        : nullptr;
}

size_t page_size()
{
    const long value = sysconf(_SC_PAGESIZE);
    return value > 0 ? static_cast<size_t>(value) : static_cast<size_t>(4096);
}

size_t align_up(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

bool align_up_checked(size_t value, size_t alignment, size_t* out)
{
    if (alignment == 0 || out == nullptr ||
        value > std::numeric_limits<size_t>::max() - (alignment - 1)) {
        return false;
    }
    *out = align_up(value, alignment);
    return true;
}

void retain_task(C_CoroTaskInternal* task)
{
    task->ref_count.fetch_add(1, std::memory_order_relaxed);
}

void release_stack(C_CoroTaskInternal* task)
{
    if (task->stack_mapping != nullptr && task->stack_mapping_size != 0) {
        munmap(task->stack_mapping, task->stack_mapping_size);
    }
    task->stack_mapping = nullptr;
    task->stack_mapping_size = 0;
    task->stack_usable = nullptr;
    task->stack_usable_size = 0;
}

void release_task(C_CoroTaskInternal* task)
{
    if (task == nullptr) {
        return;
    }
    if (task->ref_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        release_stack(task);
        delete task;
    }
}

void complete_task(C_CoroTaskInternal* task, C_CoroState state, C_IOResult result)
{
    {
        std::lock_guard<std::mutex> lock(task->mutex);
        task->state.store(state, std::memory_order_release);
        task->result = result;
    }
    task->cv.notify_all();
}

bool allocate_stack(C_CoroTaskInternal* task, size_t requested_size)
{
    const size_t guard_size = page_size();
    size_t usable_size = 0;
    if (!align_up_checked(std::max(requested_size, kMinStackSize), guard_size, &usable_size) ||
        usable_size > std::numeric_limits<size_t>::max() - guard_size) {
        task->result = make_result(C_IOResultInvalid);
        return false;
    }
    const size_t total_size = guard_size + usable_size;
    void* mapping = mmap(nullptr, total_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mapping == MAP_FAILED) {
        task->result = make_result(C_IOResultError, errno);
        return false;
    }
    if (mprotect(mapping, guard_size, PROT_NONE) != 0) {
        const int saved_errno = errno;
        munmap(mapping, total_size);
        task->result = make_result(C_IOResultError, saved_errno);
        return false;
    }

    task->stack_mapping = mapping;
    task->stack_mapping_size = total_size;
    task->stack_usable = static_cast<unsigned char*>(mapping) + guard_size;
    task->stack_usable_size = usable_size;
    return true;
}

bool init_context(C_CoroTaskInternal* task)
{
#if GALAY_C_CORO_HAS_CONTEXT
    auto stack_top = reinterpret_cast<uintptr_t>(
        static_cast<unsigned char*>(task->stack_usable) + task->stack_usable_size);
    stack_top &= ~static_cast<uintptr_t>(0x0f);
    task->context.sp = stack_top;
    task->context.pc = reinterpret_cast<uintptr_t>(&galay_coro_context_entry);
    task->context.x19 = reinterpret_cast<uintptr_t>(task);
    return true;
#else
    (void)task;
    return false;
#endif
}

bool request_ready(C_CoroTaskInternal* task)
{
    C_CoroState state = task->state.load(std::memory_order_acquire);
    if ((state != C_CoroState::Ready && state != C_CoroState::WaitReady) ||
        task->owner == nullptr) {
        return false;
    }

    bool expected = false;
    if (!task->queued.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return true;
    }

    retain_task(task);
    galay::kernel::detail::ReadyEntry entry(galay::kernel::detail::ReadyEntryKind::CCoroutine, task);
    if (!galay::kernel::detail::scheduleReadyEntry(entry)) {
        task->queued.store(false, std::memory_order_release);
        galay::kernel::detail::releaseReadyEntry(entry);
        return false;
    }
    return true;
}

galay::kernel::Scheduler* ready_owner(void* state) noexcept
{
    return static_cast<C_CoroTaskInternal*>(state)->owner;
}

bool ready_owner_only(void*) noexcept
{
    return true;
}

bool ready_resume(void* state) noexcept
{
#if GALAY_C_CORO_HAS_CONTEXT
    auto* task = static_cast<C_CoroTaskInternal*>(state);
    const std::thread::id current_thread = std::this_thread::get_id();
    const std::thread::id owner_thread =
        task->owner != nullptr ? task->owner->threadId() : std::thread::id{};
    if (owner_thread != std::thread::id{} && current_thread != owner_thread) {
        task->queued.store(false, std::memory_order_release);
        (void)request_ready(task);
        return true;
    }
    task->queued.store(false, std::memory_order_release);

    C_CoroState resumed_from = C_CoroState::Ready;
    C_CoroState expected = C_CoroState::Ready;
    if (!task->state.compare_exchange_strong(expected, C_CoroState::Running,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
        expected = C_CoroState::WaitReady;
        if (task->state.compare_exchange_strong(expected, C_CoroState::Running,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            resumed_from = C_CoroState::WaitReady;
        } else {
            if (expected == C_CoroState::Done || expected == C_CoroState::Cancelled) {
                task->cv.notify_all();
            }
            return true;
        }
    }

    if (current_thread != task->owner_thread) {
        task->state.store(resumed_from, std::memory_order_release);
        (void)request_ready(task);
        return true;
    }

    C_CoroState current = task->state.load(std::memory_order_acquire);
    if (current == C_CoroState::Done || current == C_CoroState::Cancelled) {
        task->cv.notify_all();
        return true;
    }

    t_current_task = task;
    galay_coro_context_switch(&task->scheduler_context, &task->context);
    t_current_task = nullptr;
    return true;
#else
    (void)state;
    return false;
#endif
}

void ready_release(void* state) noexcept
{
    release_task(static_cast<C_CoroTaskInternal*>(state));
}

C_CoroTaskInternal* token_task(void* state) noexcept
{
    auto* token = static_cast<C_CoroResumeTokenState*>(state);
    return token != nullptr ? token->task : nullptr;
}

galay::kernel::Scheduler* token_owner(void* state) noexcept
{
    auto* task = token_task(state);
    return task != nullptr ? task->owner : nullptr;
}

bool token_request_resume(void* state) noexcept
{
    auto* task = token_task(state);
    return task != nullptr && request_ready(task);
}

void token_retain(void* state) noexcept
{
    if (auto* task = token_task(state)) {
        retain_task(task);
    }
}

void token_release(void* state) noexcept
{
    release_task(token_task(state));
}

constexpr galay::kernel::detail::ReadyEntryHooks kReadyHooks{
    .owner_scheduler = ready_owner,
    .resume_owner_only = ready_owner_only,
    .resume = ready_resume,
    .release = ready_release,
};

constexpr galay::kernel::detail::ResumeTokenHooks kResumeHooks{
    .owner_scheduler = token_owner,
    .request_resume = token_request_resume,
    .retain = token_retain,
    .release = token_release,
};

} // namespace

extern "C" void galay_coro_context_trampoline(C_CoroTaskInternal* task)
{
#if GALAY_C_CORO_HAS_CONTEXT
    try {
        task->entry(task->arg);
        complete_task(task, C_CoroState::Done, make_result(C_IOResultOk));
    } catch (...) {
        complete_task(task, C_CoroState::Done, make_result(C_IOResultError));
    }
    t_current_task = nullptr;
    galay_coro_context_switch(&task->context, &task->scheduler_context);
    for (;;) {
        galay_coro_context_switch(&task->context, &task->scheduler_context);
    }
#else
    (void)task;
#endif
}

extern "C" {

C_CoroOptions galay_coro_options_default(void)
{
    return C_CoroOptions{kDefaultStackSize};
}

C_IOResult galay_coro_spawn(galay_kernel_runtime_t* runtime,
                            galay_coro_entry_fn entry,
                            void* arg,
                            const C_CoroOptions* options,
                            galay_coro_task_t* out_task)
{
    C_CoroTaskInternal* task = nullptr;
    try {
        auto* cpp_runtime = to_cpp_runtime(runtime);
        if (cpp_runtime == nullptr || entry == nullptr || out_task == nullptr ||
            !galay_kernel_runtime_is_running(runtime)) {
            return make_result(C_IOResultInvalid);
        }
#if !GALAY_C_CORO_HAS_CONTEXT
        (void)arg;
        (void)options;
        return make_result(C_IOResultError, ENOTSUP);
#else
        auto* owner = cpp_runtime->getNextIOScheduler();
        if (owner == nullptr) {
            return make_result(C_IOResultInvalid);
        }
        const std::thread::id owner_thread = owner->threadId();
        if (owner_thread == std::thread::id{}) {
            return make_result(C_IOResultError, EAGAIN);
        }

        task = new (std::nothrow) C_CoroTaskInternal();
        if (task == nullptr) {
            return make_result(C_IOResultError, ENOMEM);
        }
        task->ready_header.hooks = &kReadyHooks;
        task->resume_token.header.hooks = &kResumeHooks;
        task->resume_token.task = task;
        task->owner = owner;
        task->owner_thread = owner_thread;
        task->entry = entry;
        task->arg = arg;

        const size_t stack_size = options != nullptr && options->stack_size != 0
            ? options->stack_size
            : kDefaultStackSize;
        if (!allocate_stack(task, stack_size) || !init_context(task)) {
            C_IOResult result = task->result.code == C_IOResultOk
                ? make_result(C_IOResultError, ENOTSUP)
                : task->result;
            release_task(task);
            return result;
        }

        if (!request_ready(task)) {
            release_task(task);
            return make_result(C_IOResultError, EAGAIN);
        }
        out_task->task = task;
        task = nullptr;
        return make_result(C_IOResultOk);
#endif
    } catch (...) {
        release_task(task);
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_yield(void)
{
    try {
#if GALAY_C_CORO_HAS_CONTEXT
        C_CoroTaskInternal* task = t_current_task;
        if (task == nullptr || task->state.load(std::memory_order_acquire) != C_CoroState::Running) {
            return make_result(C_IOResultInvalid);
        }

        task->state.store(C_CoroState::Ready, std::memory_order_release);
        if (!request_ready(task)) {
            C_CoroState expected = C_CoroState::Ready;
            if (!task->state.compare_exchange_strong(expected, C_CoroState::Running,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
                t_current_task = nullptr;
                galay_coro_context_switch(&task->context, &task->scheduler_context);
                t_current_task = task;
                return expected == C_CoroState::Cancelled
                    ? make_result(C_IOResultCancelled)
                    : make_result(C_IOResultInvalid);
            }
            return make_result(C_IOResultError, EAGAIN);
        }
        t_current_task = nullptr;
        galay_coro_context_switch(&task->context, &task->scheduler_context);
        t_current_task = task;
        return make_result(C_IOResultOk);
#else
        return make_result(C_IOResultInvalid);
#endif
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_current(galay_coro_task_t* out_task)
{
    try {
        if (out_task == nullptr || out_task->task != nullptr || t_current_task == nullptr) {
            return make_result(C_IOResultInvalid);
        }
        retain_task(t_current_task);
        out_task->task = t_current_task;
        return make_result(C_IOResultOk);
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_join(galay_coro_task_t* task_handle, int64_t timeout_ms)
{
    try {
        if (task_handle == nullptr || task_handle->task == nullptr) {
            return make_result(C_IOResultInvalid);
        }
        auto* task = static_cast<C_CoroTaskInternal*>(task_handle->task);
        if (t_current_task != nullptr || galay::kernel::detail::isSchedulerThread()) {
            return make_result(C_IOResultInvalid);
        }
        std::unique_lock<std::mutex> lock(task->mutex);
        auto is_complete = [&]() {
            C_CoroState state = task->state.load(std::memory_order_acquire);
            return state == C_CoroState::Done || state == C_CoroState::Cancelled;
        };

        if (!is_complete()) {
            if (timeout_ms < 0) {
                task->cv.wait(lock, is_complete);
            } else if (timeout_ms == 0) {
                return make_result(C_IOResultTimeout);
            } else if (!task->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), is_complete)) {
                return make_result(C_IOResultTimeout);
            }
        }

        C_CoroState state = task->state.load(std::memory_order_acquire);
        if (state == C_CoroState::Cancelled) {
            return make_result(C_IOResultCancelled);
        }
        return task->result;
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_cancel(galay_coro_task_t* task_handle)
{
    try {
        if (task_handle == nullptr || task_handle->task == nullptr) {
            return make_result(C_IOResultInvalid);
        }
        auto* task = static_cast<C_CoroTaskInternal*>(task_handle->task);
        C_CoroState expected = C_CoroState::Ready;
        if (task->state.compare_exchange_strong(expected, C_CoroState::Cancelled,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            {
                std::lock_guard<std::mutex> lock(task->mutex);
                task->result = make_result(C_IOResultCancelled);
            }
            task->cv.notify_all();
            return make_result(C_IOResultCancelled);
        }
        if (expected == C_CoroState::Cancelled) {
            return make_result(C_IOResultCancelled);
        }
        if (expected == C_CoroState::Done) {
            std::lock_guard<std::mutex> lock(task->mutex);
            return task->result;
        }
        return make_result(C_IOResultInvalid);
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

C_IOResult galay_coro_destroy(galay_coro_task_t* task_handle)
{
    try {
        if (task_handle == nullptr || task_handle->task == nullptr) {
            return make_result(C_IOResultInvalid);
        }
        auto* task = static_cast<C_CoroTaskInternal*>(task_handle->task);
        C_CoroState state = task->state.load(std::memory_order_acquire);
        if (state != C_CoroState::Done && state != C_CoroState::Cancelled) {
            return make_result(C_IOResultInvalid);
        }
        task_handle->task = nullptr;
        release_task(task);
        return make_result(C_IOResultOk);
    } catch (...) {
        return make_result(C_IOResultError);
    }
}

} // extern "C"

namespace galay::kernel::coro_c
{

detail::ResumeToken makeResumeToken(galay_coro_task_t task) noexcept
{
    if (task.task == nullptr) {
        return {};
    }
    auto* internal = static_cast<C_CoroTaskInternal*>(task.task);
    return detail::ResumeToken::fromCCoroutine(&internal->resume_token);
}

C_CoroTaskInternal* currentTask() noexcept
{
    return t_current_task;
}

void retainTask(C_CoroTaskInternal* task) noexcept
{
    if (task != nullptr) {
        retain_task(task);
    }
}

void releaseTask(C_CoroTaskInternal* task) noexcept
{
    release_task(task);
}

bool prepareCurrentTaskWait() noexcept
{
#if GALAY_C_CORO_HAS_CONTEXT
    C_CoroTaskInternal* task = t_current_task;
    if (task == nullptr) {
        return false;
    }

    C_CoroState expected = C_CoroState::Running;
    return task->state.compare_exchange_strong(expected, C_CoroState::Waiting,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire);
#else
    return false;
#endif
}

bool rollbackCurrentTaskWait() noexcept
{
#if GALAY_C_CORO_HAS_CONTEXT
    C_CoroTaskInternal* task = t_current_task;
    if (task == nullptr) {
        return false;
    }
    C_CoroState expected = C_CoroState::Waiting;
    return task->state.compare_exchange_strong(expected, C_CoroState::Running,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire);
#else
    return false;
#endif
}

C_IOResult parkPreparedCurrentTaskWait() noexcept
{
#if GALAY_C_CORO_HAS_CONTEXT
    C_CoroTaskInternal* task = t_current_task;
    if (task == nullptr) {
        return C_IOResult{C_IOResultInvalid, 0, 0, 0, nullptr};
    }
    C_CoroState state = task->state.load(std::memory_order_acquire);
    if (state != C_CoroState::Waiting && state != C_CoroState::WaitReady) {
        return C_IOResult{C_IOResultInvalid, 0, 0, 0, nullptr};
    }

    t_current_task = nullptr;
    galay_coro_context_switch(&task->context, &task->scheduler_context);
    t_current_task = task;
    return C_IOResult{C_IOResultOk, 0, 0, 0, nullptr};
#else
    return C_IOResult{C_IOResultInvalid, 0, 0, 0, nullptr};
#endif
}

bool resumeTaskFromWait(C_CoroTaskInternal* task) noexcept
{
    if (task == nullptr) {
        return false;
    }
    C_CoroState expected = C_CoroState::Waiting;
    if (!task->state.compare_exchange_strong(expected, C_CoroState::WaitReady,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire)) {
        return expected == C_CoroState::WaitReady;
    }
    if (request_ready(task)) {
        return true;
    }
    expected = C_CoroState::WaitReady;
    (void)task->state.compare_exchange_strong(expected, C_CoroState::Waiting,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    return false;
}

} // namespace galay::kernel::coro_c
