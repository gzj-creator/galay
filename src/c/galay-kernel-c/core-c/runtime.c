#include "runtime_internal.h"

#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

static size_t resolve_scheduler_count(size_t requested)
{
    if (requested != C_RUNTIME_SCHEDULER_COUNT_AUTO) {
        return requested;
    }
    const long online = sysconf(_SC_NPROCESSORS_ONLN);
    return online > 0 ? (size_t)online : 1U;
}

static void* scheduler_thread_main(void* arg)
{
    galay_c_io_scheduler_t* const scheduler = arg;
    const C_IOResult result = galay_c_io_scheduler_run(scheduler);
    return result.code == C_IOResultOk ? NULL : scheduler;
}

const char* galay_c_runtime_get_error(C_RuntimeResultCode code)
{
    switch (code) {
    case C_RuntimeSuccess:
        return "success";
    case C_RuntimeParameterInvalid:
        return "parameter invalid";
    case C_RuntimeMemoryAllocFailed:
        return "memory allocation failed";
    case C_RuntimeStartFailed:
        return "runtime start failed";
    }
    return "unknown runtime error";
}

C_RuntimeConfig galay_c_runtime_config_default(void)
{
    return (C_RuntimeConfig){C_RUNTIME_SCHEDULER_COUNT_AUTO, 0};
}

C_RuntimeResultCode galay_c_runtime_create(const C_RuntimeConfig* config,
                                                 galay_c_runtime_t* c_runtime)
{
    if (config == NULL || c_runtime == NULL || c_runtime->runtime != NULL) {
        return C_RuntimeParameterInvalid;
    }
    const size_t scheduler_count = resolve_scheduler_count(config->io_scheduler_count);
    if (scheduler_count == 0 || scheduler_count > SIZE_MAX / sizeof(galay_c_io_scheduler_t)) {
        return C_RuntimeParameterInvalid;
    }

    galay_c_runtime_impl_t* const runtime = calloc(1, sizeof(*runtime));
    if (runtime == NULL) {
        return C_RuntimeMemoryAllocFailed;
    }
    runtime->schedulers = calloc(scheduler_count, sizeof(*runtime->schedulers));
    runtime->threads = calloc(scheduler_count, sizeof(*runtime->threads));
    if (runtime->schedulers == NULL || runtime->threads == NULL) {
        free(runtime->threads);
        free(runtime->schedulers);
        free(runtime);
        return C_RuntimeMemoryAllocFailed;
    }

    runtime->scheduler_count = scheduler_count;
    atomic_init(&runtime->next_scheduler, 0);
    atomic_init(&runtime->running, 0);
    for (size_t index = 0; index < scheduler_count; ++index) {
        const C_IOResult result = galay_c_io_scheduler_create(&runtime->schedulers[index], NULL);
        if (result.code != C_IOResultOk) {
            int cleanup_failed = 0;
            while (index != 0) {
                --index;
                const C_IOResult destroyed =
                    galay_c_io_scheduler_destroy(&runtime->schedulers[index]);
                if (destroyed.code != C_IOResultOk) {
                    cleanup_failed = 1;
                }
            }
            free(runtime->threads);
            free(runtime->schedulers);
            free(runtime);
            return cleanup_failed ? C_RuntimeStartFailed : C_RuntimeMemoryAllocFailed;
        }
    }
    c_runtime->runtime = runtime;
    return C_RuntimeSuccess;
}

C_RuntimeResultCode galay_c_runtime_start(galay_c_runtime_t* c_runtime)
{
    if (c_runtime == NULL || c_runtime->runtime == NULL) {
        return C_RuntimeParameterInvalid;
    }
    galay_c_runtime_impl_t* const runtime = c_runtime->runtime;
    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&runtime->running, &expected, 1,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return C_RuntimeSuccess;
    }
    for (size_t index = 0; index < runtime->scheduler_count; ++index) {
        if (pthread_create(&runtime->threads[index], NULL, scheduler_thread_main,
                           &runtime->schedulers[index]) != 0) {
            atomic_store_explicit(&runtime->running, 0, memory_order_release);
            for (size_t stopped = 0; stopped < index; ++stopped) {
                const C_IOResult stop_result =
                    galay_c_io_scheduler_stop(&runtime->schedulers[stopped]);
                if (stop_result.code != C_IOResultOk) {
                    // Continue unwinding every thread; startup already failed.
                    continue;
                }
            }
            for (size_t joined = 0; joined < index; ++joined) {
                void* thread_result = NULL;
                if (pthread_join(runtime->threads[joined], &thread_result) != 0 ||
                    thread_result != NULL) {
                    // Continue joining the remaining successfully started threads.
                    continue;
                }
            }
            return C_RuntimeStartFailed;
        }
        while (!galay_c_io_scheduler_is_running(&runtime->schedulers[index])) {
            sched_yield();
        }
    }
    return C_RuntimeSuccess;
}

C_RuntimeResultCode galay_c_runtime_stop(galay_c_runtime_t* c_runtime)
{
    if (c_runtime == NULL || c_runtime->runtime == NULL) {
        return C_RuntimeParameterInvalid;
    }
    galay_c_runtime_impl_t* const runtime = c_runtime->runtime;
    int expected = 1;
    if (!atomic_compare_exchange_strong_explicit(&runtime->running, &expected, 0,
                                                 memory_order_acq_rel,
                                                 memory_order_acquire)) {
        return C_RuntimeSuccess;
    }
    int failed = 0;
    for (size_t index = 0; index < runtime->scheduler_count; ++index) {
        const C_IOResult result = galay_c_io_scheduler_stop(&runtime->schedulers[index]);
        if (result.code != C_IOResultOk) {
            failed = 1;
        }
    }
    for (size_t index = 0; index < runtime->scheduler_count; ++index) {
        void* thread_result = NULL;
        if (pthread_join(runtime->threads[index], &thread_result) != 0 ||
            thread_result != NULL) {
            failed = 1;
        }
    }
    return failed ? C_RuntimeStartFailed : C_RuntimeSuccess;
}

bool galay_c_runtime_is_running(const galay_c_runtime_t* c_runtime)
{
    if (c_runtime == NULL || c_runtime->runtime == NULL) {
        return false;
    }
    const galay_c_runtime_impl_t* const runtime = c_runtime->runtime;
    return atomic_load_explicit(&runtime->running, memory_order_acquire) != 0;
}

C_RuntimeResultCode galay_c_runtime_destroy(galay_c_runtime_t* c_runtime)
{
    if (c_runtime == NULL) {
        return C_RuntimeParameterInvalid;
    }
    if (c_runtime->runtime == NULL) {
        return C_RuntimeSuccess;
    }
    galay_c_runtime_impl_t* const runtime = c_runtime->runtime;
    if (atomic_load_explicit(&runtime->running, memory_order_acquire) != 0) {
        return C_RuntimeParameterInvalid;
    }
    for (size_t index = 0; index < runtime->scheduler_count; ++index) {
        if (galay_c_io_scheduler_is_running(&runtime->schedulers[index])) {
            return C_RuntimeParameterInvalid;
        }
    }
    for (size_t index = 0; index < runtime->scheduler_count; ++index) {
        const C_IOResult destroyed =
            galay_c_io_scheduler_destroy(&runtime->schedulers[index]);
        if (destroyed.code != C_IOResultOk) {
            return C_RuntimeStartFailed;
        }
    }
    free(runtime->threads);
    free(runtime->schedulers);
    free(runtime);
    c_runtime->runtime = NULL;
    return C_RuntimeSuccess;
}

galay_c_io_scheduler_t* galay_c_runtime_next_scheduler(galay_c_runtime_t* runtime)
{
    if (runtime == NULL || runtime->runtime == NULL) {
        return NULL;
    }
    galay_c_runtime_impl_t* const impl = runtime->runtime;
    if (atomic_load_explicit(&impl->running, memory_order_acquire) == 0 ||
        impl->scheduler_count == 0) {
        return NULL;
    }
    const size_t next = atomic_fetch_add_explicit(&impl->next_scheduler, 1,
                                                  memory_order_relaxed);
    return &impl->schedulers[next % impl->scheduler_count];
}
