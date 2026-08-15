#ifndef GALAY_C_KERNEL_CORO_TASK_INTERNAL_H
#define GALAY_C_KERNEL_CORO_TASK_INTERNAL_H

#include "coro_task.h"

#include <stdatomic.h>

typedef struct galay_c_io_scheduler galay_c_io_scheduler_t;
typedef struct galay_c_io_controller galay_c_io_controller_t;

typedef struct C_CoroMachineContext {
#if defined(__APPLE__) && defined(__aarch64__)
    uint64_t x19;
    uint64_t x20;
    uint64_t x21;
    uint64_t x22;
    uint64_t x23;
    uint64_t x24;
    uint64_t x25;
    uint64_t x26;
    uint64_t x27;
    uint64_t x28;
    uint64_t x29;
    uint64_t pc;
    uint64_t sp;
    uint64_t d8;
    uint64_t d9;
    uint64_t d10;
    uint64_t d11;
    uint64_t d12;
    uint64_t d13;
    uint64_t d14;
    uint64_t d15;
#elif defined(__linux__) && defined(__x86_64__)
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;
    uint64_t rsp;
#else
    uint64_t reserved;
#endif
} C_CoroMachineContext;

typedef enum C_CoroState {
    C_CoroStateReady,
    C_CoroStateRunning,
    C_CoroStateWaiting,
    C_CoroStateCompleted,
    C_CoroStateCancelled,
} C_CoroState;

typedef struct C_CoroTaskInternal {
    galay_c_io_scheduler_t* owner;
    galay_c_coro_entry_fn entry;
    void* arg;
    C_CoroMachineContext scheduler_context;
    C_CoroMachineContext context;
    void* stack_mapping;
    size_t stack_mapping_size;
    void* stack_usable;
    size_t stack_usable_size;
    C_IOResult result;
    struct C_CoroTaskInternal* timeout_next;
    galay_c_io_controller_t* wait_controller;
    int64_t wait_deadline_ms;
    _Atomic uint32_t ref_count;
    _Atomic C_CoroState state;
    _Atomic C_IOResultCode wait_code;
    _Atomic int queued;
    uint32_t wait_event;
    int timeout_active;
} C_CoroTaskInternal;

C_CoroTaskInternal* galay_c_coro_task_current_internal(void);
void galay_c_coro_task_retain(C_CoroTaskInternal* task);
void galay_c_coro_task_release(C_CoroTaskInternal* task);
C_IOResult galay_c_coro_task_enqueue(C_CoroTaskInternal* task);
C_IOResult galay_c_coro_task_wake(C_CoroTaskInternal* task);
C_IOResult galay_c_coro_task_prepare_wait(void);
C_IOResult galay_c_coro_task_rollback_wait(void);
C_IOResult galay_c_coro_task_park_prepared(void);
C_IOResult galay_c_coro_task_suspend_current(C_CoroState next_state);
/** Scheduler-thread-only deadline registration used by native I/O waits. */
C_IOResult galay_c_coro_task_register_timeout(C_CoroTaskInternal* task,
                                            galay_c_io_controller_t* controller,
                                            uint32_t event_type,
                                            int64_t timeout_ms);
void galay_c_coro_task_cancel_timeout(C_CoroTaskInternal* task);
void galay_c_coro_task_process_timeouts(galay_c_io_scheduler_t* scheduler);
int galay_c_coro_task_next_timeout_ms(galay_c_io_scheduler_t* scheduler,
                                    int maximum_ms);
void galay_c_coro_task_resume(C_CoroTaskInternal* task);
int galay_c_coro_task_is_current_thread(void);

#endif
