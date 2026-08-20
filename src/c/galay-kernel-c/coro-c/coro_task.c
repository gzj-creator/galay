#include "coro_task_internal.h"

#include "../core-c/io_scheduler.h"
#include "../core-c/runtime_internal.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/mman.h>
#include <unistd.h>

#if (defined(__APPLE__) && defined(__aarch64__)) || \
    (defined(__linux__) && defined(__x86_64__))
#define GALAY_C_CORO_HAS_CONTEXT 1
#else
#define GALAY_C_CORO_HAS_CONTEXT 0
#endif

enum {
    GALAY_C_CORO_DEFAULT_STACK_SIZE = 64 * 1024,
    GALAY_C_CORO_MIN_STACK_SIZE = 16 * 1024,
};

static _Thread_local C_CoroTaskInternal* galay_c_coro_current_task;

#if GALAY_C_CORO_HAS_CONTEXT
extern void galay_c_coro_context_switch(C_CoroMachineContext* from,
                                      const C_CoroMachineContext* to);
extern void galay_c_coro_context_entry(void);
#endif

static C_IOResult make_result(C_IOResultCode code, int sys_errno)
{
    return (C_IOResult){code, sys_errno, 0, 0, NULL};
}

static size_t page_size(void)
{
    const long value = sysconf(_SC_PAGESIZE);
    return value > 0 ? (size_t)value : 4096U;
}

static int align_up(size_t value, size_t alignment, size_t* out_value)
{
    if (out_value == NULL || alignment == 0 ||
        value > SIZE_MAX - (alignment - 1U)) {
        return 0;
    }
    *out_value = (value + alignment - 1U) & ~(alignment - 1U);
    return 1;
}

static void release_stack(C_CoroTaskInternal* task)
{
    if (task->stack_mapping != NULL && task->stack_mapping_size != 0) {
        // Final release has no caller-visible error channel; a failed unmap remains process-owned.
        (void)munmap(task->stack_mapping, task->stack_mapping_size);
    }
    task->stack_mapping = NULL;
    task->stack_mapping_size = 0;
    task->stack_usable = NULL;
    task->stack_usable_size = 0;
}

void galay_c_coro_task_retain(C_CoroTaskInternal* task)
{
    if (task != NULL) {
        (void)atomic_fetch_add_explicit(&task->ref_count, 1, memory_order_relaxed);
    }
}

void galay_c_coro_task_release(C_CoroTaskInternal* task)
{
    if (task != NULL &&
        atomic_fetch_sub_explicit(&task->ref_count, 1, memory_order_acq_rel) == 1) {
        release_stack(task);
        free(task);
    }
}

static int allocate_stack(C_CoroTaskInternal* task, size_t requested_size)
{
    const size_t guard_size = page_size();
    const size_t requested = requested_size < GALAY_C_CORO_MIN_STACK_SIZE
        ? GALAY_C_CORO_MIN_STACK_SIZE
        : requested_size;
    size_t usable_size = 0;
    if (!align_up(requested, guard_size, &usable_size) ||
        usable_size > SIZE_MAX - guard_size) {
        task->result = make_result(C_IOResultInvalid, 0);
        return 0;
    }

    const size_t total_size = usable_size + guard_size;
    void* mapping = mmap(NULL, total_size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
        task->result = make_result(C_IOResultError, errno);
        return 0;
    }
    if (mprotect(mapping, guard_size, PROT_NONE) != 0) {
        const int saved_errno = errno;
        // Preserve the mprotect failure that made the stack unusable.
        (void)munmap(mapping, total_size);
        task->result = make_result(C_IOResultError, saved_errno);
        return 0;
    }

    task->stack_mapping = mapping;
    task->stack_mapping_size = total_size;
    task->stack_usable = (unsigned char*)mapping + guard_size;
    task->stack_usable_size = usable_size;
    return 1;
}

static int init_context(C_CoroTaskInternal* task)
{
#if GALAY_C_CORO_HAS_CONTEXT
    uintptr_t stack_top = (uintptr_t)((unsigned char*)task->stack_usable +
                                      task->stack_usable_size);
    stack_top &= ~(uintptr_t)0x0f;
#if defined(__APPLE__) && defined(__aarch64__)
    task->context.sp = stack_top;
    task->context.pc = (uintptr_t)&galay_c_coro_context_entry;
    task->context.x19 = (uintptr_t)task;
#elif defined(__linux__) && defined(__x86_64__)
    task->context.rsp = stack_top;
    task->context.rip = (uintptr_t)&galay_c_coro_context_entry;
    task->context.r12 = (uintptr_t)task;
#endif
    return 1;
#else
    (void)task;
    return 0;
#endif
}

C_CoroTaskInternal* galay_c_coro_task_current_internal(void)
{
    return galay_c_coro_current_task;
}

int galay_c_coro_task_is_current_thread(void)
{
    return galay_c_coro_current_task != NULL;
}

C_IOResult galay_c_coro_task_enqueue(C_CoroTaskInternal* task)
{
    if (task == NULL || task->owner == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }

    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&task->queued, &expected, 1,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        return make_result(C_IOResultOk, 0);
    }

    galay_c_coro_task_retain(task);
    const C_IOResult result = galay_c_io_scheduler_enqueue_ready(task->owner, task);
    if (result.code != C_IOResultOk) {
        atomic_store_explicit(&task->queued, 0, memory_order_release);
        galay_c_coro_task_release(task);
    }
    return result;
}

C_IOResult galay_c_coro_task_wake(C_CoroTaskInternal* task)
{
    if (task == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    C_CoroState expected = C_CoroStateWaiting;
    if (!atomic_compare_exchange_strong_explicit(&task->state, &expected, C_CoroStateReady,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        return make_result(C_IOResultInvalid, 0);
    }
    return galay_c_coro_task_enqueue(task);
}

C_IOResult galay_c_coro_task_suspend_current(C_CoroState next_state)
{
#if GALAY_C_CORO_HAS_CONTEXT
    C_CoroTaskInternal* const task = galay_c_coro_current_task;
    if (task == NULL ||
        atomic_load_explicit(&task->state, memory_order_acquire) != C_CoroStateRunning) {
        return make_result(C_IOResultInvalid, 0);
    }
    atomic_store_explicit(&task->state, next_state, memory_order_release);
    galay_c_coro_current_task = NULL;
    galay_c_coro_context_switch(&task->context, &task->scheduler_context);
    galay_c_coro_current_task = task;
    return make_result(C_IOResultOk, 0);
#else
    (void)next_state;
    return make_result(C_IOResultError, ENOTSUP);
#endif
}

C_IOResult galay_c_coro_task_prepare_wait(void)
{
    C_CoroTaskInternal* const task = galay_c_coro_current_task;
    if (task == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    C_CoroState expected = C_CoroStateRunning;
    return atomic_compare_exchange_strong_explicit(&task->state, &expected,
                                                    C_CoroStateWaiting,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)
        ? make_result(C_IOResultOk, 0)
        : make_result(C_IOResultInvalid, 0);
}

C_IOResult galay_c_coro_task_rollback_wait(void)
{
    C_CoroTaskInternal* const task = galay_c_coro_current_task;
    if (task == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    C_CoroState expected = C_CoroStateWaiting;
    return atomic_compare_exchange_strong_explicit(&task->state, &expected,
                                                    C_CoroStateRunning,
                                                    memory_order_acq_rel,
                                                    memory_order_acquire)
        ? make_result(C_IOResultOk, 0)
        : make_result(C_IOResultInvalid, 0);
}

C_IOResult galay_c_coro_task_park_prepared(void)
{
#if GALAY_C_CORO_HAS_CONTEXT
    C_CoroTaskInternal* const task = galay_c_coro_current_task;
    const C_CoroState state = task != NULL
        ? atomic_load_explicit(&task->state, memory_order_acquire)
        : C_CoroStateCompleted;
    if (task == NULL || (state != C_CoroStateWaiting && state != C_CoroStateReady)) {
        return make_result(C_IOResultInvalid, 0);
    }
    galay_c_coro_current_task = NULL;
    galay_c_coro_context_switch(&task->context, &task->scheduler_context);
    galay_c_coro_current_task = task;
    return make_result(C_IOResultOk, 0);
#else
    return make_result(C_IOResultError, ENOTSUP);
#endif
}

void galay_c_coro_task_resume(C_CoroTaskInternal* task)
{
#if GALAY_C_CORO_HAS_CONTEXT
    if (task == NULL) {
        return;
    }
    atomic_store_explicit(&task->queued, 0, memory_order_release);
    C_CoroState expected = C_CoroStateReady;
    if (!atomic_compare_exchange_strong_explicit(&task->state, &expected, C_CoroStateRunning,
                                                  memory_order_acq_rel,
                                                  memory_order_acquire)) {
        return;
    }
    galay_c_coro_current_task = task;
    galay_c_coro_context_switch(&task->scheduler_context, &task->context);
    galay_c_coro_current_task = NULL;
#else
    (void)task;
#endif
}

void galay_c_coro_context_trampoline(C_CoroTaskInternal* task)
{
#if GALAY_C_CORO_HAS_CONTEXT
    task->entry(task->arg);
    task->result = make_result(C_IOResultOk, 0);
    atomic_store_explicit(&task->state, C_CoroStateCompleted, memory_order_release);
    galay_c_coro_current_task = NULL;
    galay_c_coro_context_switch(&task->context, &task->scheduler_context);
    for (;;) {
        galay_c_coro_context_switch(&task->context, &task->scheduler_context);
    }
#else
    (void)task;
#endif
}

C_CoroOptions galay_c_coro_options_default(void)
{
    return (C_CoroOptions){GALAY_C_CORO_DEFAULT_STACK_SIZE};
}

C_IOResult galay_c_coro_spawn(galay_c_runtime_t* runtime,
                            galay_c_coro_entry_fn entry,
                            void* arg,
                            const C_CoroOptions* options,
                            galay_c_coro_task_t* out_task)
{
    if (runtime == NULL || entry == NULL || out_task == NULL || out_task->task != NULL ||
        !galay_c_runtime_is_running(runtime)) {
        return make_result(C_IOResultInvalid, 0);
    }
#if !GALAY_C_CORO_HAS_CONTEXT
    (void)arg;
    (void)options;
    return make_result(C_IOResultError, ENOTSUP);
#else
    galay_c_io_scheduler_t* const owner = galay_c_runtime_next_scheduler(runtime);
    if (owner == NULL || !galay_c_io_scheduler_is_running(owner)) {
        return make_result(C_IOResultInvalid, 0);
    }

    C_CoroTaskInternal* const task = calloc(1, sizeof(*task));
    if (task == NULL) {
        return make_result(C_IOResultError, ENOMEM);
    }
    task->owner = owner;
    task->entry = entry;
    task->arg = arg;
    task->result = make_result(C_IOResultOk, 0);
    atomic_init(&task->ref_count, 1);
    atomic_init(&task->state, C_CoroStateReady);
    atomic_init(&task->wait_code, C_IOResultOk);
    atomic_init(&task->queued, 0);

    const size_t stack_size = options != NULL && options->stack_size != 0
        ? options->stack_size
        : GALAY_C_CORO_DEFAULT_STACK_SIZE;
    if (!allocate_stack(task, stack_size) || !init_context(task)) {
        const C_IOResult result = task->result.code == C_IOResultOk
            ? make_result(C_IOResultError, ENOTSUP)
            : task->result;
        galay_c_coro_task_release(task);
        return result;
    }

    const C_IOResult queued = galay_c_coro_task_enqueue(task);
    if (queued.code != C_IOResultOk) {
        galay_c_coro_task_release(task);
        return queued;
    }
    out_task->task = task;
    return make_result(C_IOResultOk, 0);
#endif
}

C_IOResult galay_c_coro_yield(void)
{
    C_CoroTaskInternal* const task = galay_c_coro_current_task;
    if (task == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    const C_IOResult queued = galay_c_coro_task_enqueue(task);
    if (queued.code != C_IOResultOk) {
        return queued;
    }
    return galay_c_coro_task_suspend_current(C_CoroStateReady);
}

C_IOResult galay_c_coro_current(galay_c_coro_task_t* out_task)
{
    if (out_task == NULL || out_task->task != NULL || galay_c_coro_current_task == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    galay_c_coro_task_retain(galay_c_coro_current_task);
    out_task->task = galay_c_coro_current_task;
    return make_result(C_IOResultOk, 0);
}

static int task_is_final(C_CoroState state)
{
    return state == C_CoroStateCompleted || state == C_CoroStateCancelled;
}

static int64_t monotonic_milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return -1;
    }
    if (now.tv_sec > INT64_MAX / 1000) {
        return INT64_MAX;
    }
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

C_IOResult galay_c_coro_task_register_timeout(C_CoroTaskInternal* task,
                                            galay_c_io_controller_t* controller,
                                            uint32_t event_type,
                                            int64_t timeout_ms)
{
    if (task == NULL || task->owner == NULL || controller == NULL || timeout_ms <= 0 ||
        task->timeout_active ||
        (event_type != GALAY_C_EVENT_READ && event_type != GALAY_C_EVENT_WRITE)) {
        return make_result(C_IOResultInvalid, 0);
    }
    const int64_t now = monotonic_milliseconds();
    if (now < 0) {
        return make_result(C_IOResultError, errno);
    }
    task->wait_deadline_ms = timeout_ms > INT64_MAX - now ? INT64_MAX : now + timeout_ms;
    task->wait_controller = controller;
    task->wait_event = event_type;
    task->timeout_next = task->owner->timeout_head;
    task->owner->timeout_head = task;
    task->timeout_active = 1;
    return make_result(C_IOResultOk, 0);
}

void galay_c_coro_task_cancel_timeout(C_CoroTaskInternal* task)
{
    if (task == NULL || task->owner == NULL || !task->timeout_active) {
        return;
    }
    C_CoroTaskInternal** cursor =
        (C_CoroTaskInternal**)&task->owner->timeout_head;
    while (*cursor != NULL && *cursor != task) {
        cursor = &(*cursor)->timeout_next;
    }
    if (*cursor == task) {
        *cursor = task->timeout_next;
    }
    task->timeout_next = NULL;
    task->wait_controller = NULL;
    task->wait_deadline_ms = 0;
    task->wait_event = GALAY_C_EVENT_NONE;
    task->timeout_active = 0;
}

static int task_wait_slot_matches(const C_CoroTaskInternal* task)
{
    if (task->wait_controller == NULL) {
        return 0;
    }
    C_CoroTaskInternal* const waiting = task->wait_event == GALAY_C_EVENT_READ
        ? atomic_load_explicit(&task->wait_controller->read_slot, memory_order_acquire)
        : atomic_load_explicit(&task->wait_controller->write_slot, memory_order_acquire);
    return waiting == task;
}

void galay_c_coro_task_process_timeouts(galay_c_io_scheduler_t* scheduler)
{
    if (scheduler == NULL || scheduler->timeout_head == NULL) {
        return;
    }
    const int64_t now = monotonic_milliseconds();
    if (now < 0) {
        return;
    }
    C_CoroTaskInternal** cursor = (C_CoroTaskInternal**)&scheduler->timeout_head;
    while (*cursor != NULL) {
        C_CoroTaskInternal* const task = *cursor;
        if (!task->timeout_active || !task_wait_slot_matches(task)) {
            *cursor = task->timeout_next;
            task->timeout_next = NULL;
            task->wait_controller = NULL;
            task->wait_deadline_ms = 0;
            task->wait_event = GALAY_C_EVENT_NONE;
            task->timeout_active = 0;
            continue;
        }
        if (task->wait_deadline_ms > now) {
            cursor = &task->timeout_next;
            continue;
        }

        galay_c_io_controller_t* const controller = task->wait_controller;
        const uint32_t event_type = task->wait_event;
        *cursor = task->timeout_next;
        task->timeout_next = NULL;
        task->wait_controller = NULL;
        task->wait_deadline_ms = 0;
        task->wait_event = GALAY_C_EVENT_NONE;
        task->timeout_active = 0;

        const C_IOResult cleared = event_type == GALAY_C_EVENT_READ
            ? galay_c_io_controller_clear_read(controller, task)
            : galay_c_io_controller_clear_write(controller, task);
        if (cleared.code == C_IOResultOk) {
            atomic_store_explicit(&task->wait_code, C_IOResultTimeout,
                                  memory_order_release);
            const C_IOResult woke = galay_c_coro_task_wake(task);
            if (woke.code != C_IOResultOk) {
                atomic_store_explicit(&task->wait_code, C_IOResultError,
                                      memory_order_release);
            }
            galay_c_coro_task_release(task);
        }
    }
}

int galay_c_coro_task_next_timeout_ms(galay_c_io_scheduler_t* scheduler,
                                    int maximum_ms)
{
    if (scheduler == NULL || maximum_ms < 0) {
        return 0;
    }
    const int64_t now = monotonic_milliseconds();
    if (now < 0) {
        return 0;
    }
    int next_ms = maximum_ms;
    for (C_CoroTaskInternal* task = scheduler->timeout_head;
         task != NULL;
         task = task->timeout_next) {
        if (!task->timeout_active) {
            continue;
        }
        const int64_t remaining = task->wait_deadline_ms - now;
        if (remaining <= 0) {
            return 0;
        }
        if (remaining < next_ms) {
            next_ms = (int)remaining;
        }
    }
    return next_ms;
}

C_IOResult galay_c_coro_join(galay_c_coro_task_t* task_handle, int64_t timeout_ms)
{
    if (task_handle == NULL || task_handle->task == NULL || timeout_ms < -1 ||
        galay_c_coro_task_is_current_thread() || galay_c_io_scheduler_is_current_thread()) {
        return make_result(C_IOResultInvalid, 0);
    }
    C_CoroTaskInternal* const task = task_handle->task;
    const int64_t start = monotonic_milliseconds();
    if (start < 0) {
        return make_result(C_IOResultError, errno);
    }

    for (;;) {
        const C_CoroState state = atomic_load_explicit(&task->state, memory_order_acquire);
        if (state == C_CoroStateCompleted) {
            return task->result;
        }
        if (state == C_CoroStateCancelled) {
            return make_result(C_IOResultCancelled, 0);
        }
        if (timeout_ms == 0 ||
            (timeout_ms > 0 && monotonic_milliseconds() - start >= timeout_ms)) {
            return make_result(C_IOResultTimeout, 0);
        }
        const struct timespec pause = {0, 1000000};
        if (nanosleep(&pause, NULL) != 0 && errno != EINTR) {
            return make_result(C_IOResultError, errno);
        }
    }
}

C_IOResult galay_c_coro_cancel(galay_c_coro_task_t* task_handle)
{
    if (task_handle == NULL || task_handle->task == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    C_CoroTaskInternal* const task = task_handle->task;
    C_CoroState expected = C_CoroStateReady;
    if (atomic_compare_exchange_strong_explicit(&task->state, &expected, C_CoroStateCancelled,
                                                memory_order_acq_rel,
                                                memory_order_acquire)) {
        return make_result(C_IOResultCancelled, 0);
    }
    if (task_is_final(expected)) {
        return expected == C_CoroStateCancelled
            ? make_result(C_IOResultCancelled, 0)
            : task->result;
    }
    return make_result(C_IOResultInvalid, 0);
}

C_IOResult galay_c_coro_destroy(galay_c_coro_task_t* task_handle)
{
    if (task_handle == NULL || task_handle->task == NULL) {
        return make_result(C_IOResultInvalid, 0);
    }
    C_CoroTaskInternal* const task = task_handle->task;
    if (!task_is_final(atomic_load_explicit(&task->state, memory_order_acquire))) {
        return make_result(C_IOResultInvalid, 0);
    }
    task_handle->task = NULL;
    galay_c_coro_task_release(task);
    return make_result(C_IOResultOk, 0);
}
