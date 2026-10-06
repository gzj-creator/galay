#include <galay/c/galay-kernel-c/core-c/runtime.h>
#include <galay/c/galay-kernel-c/core-c/io_scheduler.h>
#include <galay/c/galay-kernel-c/async-c/tcp_socket.h>
#include <galay/c/galay-kernel-c/coro-c/coro_sleep.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_internal.h>
#include <galay/c/galay-kernel-c/coro-c/coro_wait.h>

#include <assert.h>
#include <sched.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static void sleeper_entry(void* arg)
{
    C_IOResult* const result = arg;
    *result = galay_c_coro_sleep(INT64_MAX);
}

typedef struct WaitInput {
    galay_c_io_controller_t* controller;
    int64_t timeout_ms;
} WaitInput;

static void waiter_entry(void* arg)
{
    const WaitInput* const input = arg;
    const C_IOResult result = galay_c_coro_wait_io(
        galay_c_io_scheduler_current(), input->controller, GALAY_C_EVENT_READ, input->timeout_ms);
    assert(result.code == C_IOResultOk);
}

static void wait_until_parked(C_CoroTaskInternal* task)
{
    struct timespec start;
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    while (atomic_load_explicit(&task->state, memory_order_acquire) != C_CoroStateWaiting) {
        struct timespec now;
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        assert(now.tv_sec - start.tv_sec < 5);
        sched_yield();
    }
}

int main(void)
{
    C_RuntimeConfig config = galay_c_runtime_config_default();
    config.io_scheduler_count = 1;
    config.parallel_scheduler_count = 0;
    galay_c_runtime_t runtime = {0};
    galay_c_coro_task_t sleeper = {0};
    galay_c_coro_task_t waiter = {0};
    galay_c_coro_task_t untimed_waiter = {0};
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    galay_c_tcp_socket_t socket = {.fd = sockets[0]};
    galay_c_io_controller_t* const controller = &socket.controller;
    assert(galay_c_io_controller_init(controller, sockets[0], NULL).code == C_IOResultOk);
    galay_c_io_controller_t untimed_controller = {0};
    assert(galay_c_io_controller_init(&untimed_controller, sockets[1], NULL).code == C_IOResultOk);
    WaitInput timed_input = {controller, INT64_MAX};
    WaitInput untimed_input = {&untimed_controller, -1};
    C_IOResult sleep_result = {C_IOResultInvalid, 0, 0, 0, NULL};
    assert(galay_c_runtime_create(&config, &runtime) == C_RuntimeSuccess);
    assert(galay_c_runtime_start(&runtime) == C_RuntimeSuccess);
    assert(galay_c_coro_spawn(&runtime, sleeper_entry, &sleep_result, NULL, &sleeper).code ==
           C_IOResultOk);
    assert(galay_c_coro_spawn(&runtime, waiter_entry, &timed_input, NULL, &waiter).code ==
           C_IOResultOk);
    assert(galay_c_coro_spawn(&runtime, waiter_entry, &untimed_input, NULL, &untimed_waiter).code ==
           C_IOResultOk);
    C_CoroTaskInternal* const sleep_task = sleeper.task;
    C_CoroTaskInternal* const wait_task = waiter.task;
    socket.scheduler = wait_task->owner;
    C_CoroTaskInternal* const untimed_task = untimed_waiter.task;
    wait_until_parked(sleep_task);
    wait_until_parked(wait_task);
    wait_until_parked(untimed_task);
    assert(galay_c_io_scheduler_destroy(sleep_task->owner).code == C_IOResultInvalid);
    assert(galay_c_runtime_stop(&runtime) == C_RuntimeSuccess);
    assert(sleep_task->timeout_active);
    assert(galay_c_runtime_destroy(&runtime) == C_RuntimeSuccess);
    assert(galay_c_coro_join(&sleeper, 0).code == C_IOResultCancelled);
    assert(atomic_load_explicit(&sleep_task->ref_count, memory_order_acquire) == 1);
    assert(galay_c_coro_destroy(&sleeper).code == C_IOResultOk);
    assert(galay_c_coro_join(&waiter, 0).code == C_IOResultCancelled);
    assert(atomic_load_explicit(&wait_task->ref_count, memory_order_acquire) == 1);
    assert(atomic_load_explicit(&controller->read_slot, memory_order_acquire) == NULL);
    assert(atomic_load_explicit(&controller->registered_events, memory_order_acquire) ==
           GALAY_C_EVENT_NONE);
    assert(atomic_load_explicit(&controller->owner_scheduler, memory_order_acquire) == NULL);
    assert(galay_c_coro_destroy(&waiter).code == C_IOResultOk);
    assert(galay_c_coro_join(&untimed_waiter, 0).code == C_IOResultCancelled);
    assert(atomic_load_explicit(&untimed_task->ref_count, memory_order_acquire) == 1);
    assert(atomic_load_explicit(&untimed_controller.read_slot, memory_order_acquire) == NULL);
    assert(atomic_load_explicit(&untimed_controller.registered_events, memory_order_acquire) ==
           GALAY_C_EVENT_NONE);
    assert(atomic_load_explicit(&untimed_controller.owner_scheduler, memory_order_acquire) == NULL);
    assert(galay_c_coro_destroy(&untimed_waiter).code == C_IOResultOk);
    assert(galay_c_tcp_socket_close(&socket).code == C_IOResultOk);
    assert(galay_c_io_controller_cleanup(&untimed_controller).code == C_IOResultOk);
    assert(close(sockets[1]) == 0);

    // A stopped scheduler also owns references to tasks still in its ready queue.
    galay_c_io_scheduler_t scheduler = {0};
    assert(galay_c_io_scheduler_create(&scheduler, NULL).code == C_IOResultOk);
    C_CoroTaskInternal* const pending = calloc(1, sizeof(*pending));
    assert(pending != NULL);
    pending->owner = &scheduler;
    atomic_init(&pending->ref_count, 1);
    atomic_init(&pending->state, C_CoroStateReady);
    atomic_init(&pending->queued, 0);
    atomic_init(&pending->wait_code, C_IOResultOk);
    assert(galay_c_coro_task_enqueue(pending).code == C_IOResultOk);
    assert(galay_c_io_scheduler_destroy(&scheduler).code == C_IOResultOk);
    galay_c_coro_task_t handle = {pending};
    assert(galay_c_coro_join(&handle, 0).code == C_IOResultCancelled);
    assert(atomic_load_explicit(&pending->ref_count, memory_order_acquire) == 1);
    assert(galay_c_coro_destroy(&handle).code == C_IOResultOk);
    return 0;
}
