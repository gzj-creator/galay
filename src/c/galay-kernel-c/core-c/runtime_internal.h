#ifndef GALAY_C_KERNEL_CORE_RUNTIME_INTERNAL_H
#define GALAY_C_KERNEL_CORE_RUNTIME_INTERNAL_H

#include "runtime.h"
#include "io_scheduler.h"

#include <pthread.h>
#include <stdatomic.h>

typedef struct galay_c_runtime_impl {
    galay_c_io_scheduler_t* schedulers;
    pthread_t* threads;
    size_t scheduler_count;
    _Atomic size_t next_scheduler;
    _Atomic int running;
} galay_c_runtime_impl_t;

galay_c_io_scheduler_t* galay_c_runtime_next_scheduler(galay_c_runtime_t* runtime);

#endif
