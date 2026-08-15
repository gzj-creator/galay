#include "io_scheduler.h"
#include "../coro-c/coro_task_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#ifdef __linux__
#include <sys/epoll.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#include <sys/time.h>
#endif

#include <unistd.h>

// 简单的无锁 MPSC ready queue（多生产者单消费者）
typedef struct ready_queue_node {
    C_CoroTaskInternal* coro;
    struct ready_queue_node* next;
} ready_queue_node_t;

typedef struct ready_queue {
    _Atomic(ready_queue_node_t*) head;  // 生产者端（原子）
    ready_queue_node_t* tail;            // 消费者端（单线程）
    ready_queue_node_t* cache;           // 节点缓存池
    size_t cache_count;
} ready_queue_t;

static const int kDefaultMaxEvents = 128;
static const int kReadyQueueCacheSize = 256;

static _Thread_local galay_c_io_scheduler_t* galay_c_current_scheduler;

static C_IOResult make_result(C_IOResultCode code, int sys_errno)
{
    return (C_IOResult){code, sys_errno, 0, 0, NULL};
}

// Ready queue 实现
static ready_queue_t* ready_queue_create(void)
{
    ready_queue_t* queue = calloc(1, sizeof(ready_queue_t));
    if (!queue) {
        return NULL;
    }

    // 创建 sentinel 节点
    ready_queue_node_t* sentinel = calloc(1, sizeof(ready_queue_node_t));
    if (!sentinel) {
        free(queue);
        return NULL;
    }

    atomic_init(&queue->head, sentinel);
    queue->tail = sentinel;
    queue->cache = NULL;
    queue->cache_count = 0;

    return queue;
}

static void ready_queue_destroy(ready_queue_t* queue)
{
    if (!queue) {
        return;
    }

    // 释放所有节点
    ready_queue_node_t* node = queue->tail;
    while (node) {
        ready_queue_node_t* next = node->next;
        free(node);
        node = next;
    }

    // 释放缓存节点
    node = queue->cache;
    while (node) {
        ready_queue_node_t* next = node->next;
        free(node);
        node = next;
    }

    free(queue);
}

// 生产者端：原子入队（线程安全）
static int ready_queue_push(ready_queue_t* queue, C_CoroTaskInternal* coro)
{
    if (!queue || !coro) {
        return 0;
    }

    ready_queue_node_t* node = calloc(1, sizeof(ready_queue_node_t));
    if (!node) {
        return 0;
    }

    node->coro = coro;
    node->next = NULL;

    // 原子交换 head 指针
    ready_queue_node_t* prev = atomic_exchange_explicit(&queue->head,
                                                         node,
                                                         memory_order_acq_rel);
    // 链接前驱节点
    atomic_store_explicit(&prev->next, node, memory_order_release);

    return 1;
}

// 消费者端：批量出队（单线程，无竞争）
static size_t ready_queue_pop_batch(ready_queue_t* queue,
                                    C_CoroTaskInternal** out_coros,
                                    size_t max_count)
{
    if (!queue || !out_coros || max_count == 0) {
        return 0;
    }

    size_t count = 0;
    ready_queue_node_t* tail = queue->tail;

    while (count < max_count) {
        ready_queue_node_t* next = atomic_load_explicit(&tail->next, memory_order_acquire);
        if (!next) {
            break;
        }

        out_coros[count++] = next->coro;

        // 回收旧 tail 到缓存
        if (queue->cache_count < kReadyQueueCacheSize) {
            tail->next = queue->cache;
            queue->cache = tail;
            queue->cache_count++;
        } else {
            free(tail);
        }

        tail = next;
    }

    queue->tail = tail;
    return count;
}

static int ready_queue_has_items(const ready_queue_t* queue)
{
    return queue != NULL &&
           atomic_load_explicit(&queue->tail->next, memory_order_acquire) != NULL;
}

// Reactor context
#ifdef __linux__
typedef struct epoll_reactor_context {
    int epoll_fd;
    struct epoll_event* events;
    int max_events;
} epoll_reactor_context_t;

static void* reactor_create(int max_events)
{
    epoll_reactor_context_t* ctx = calloc(1, sizeof(epoll_reactor_context_t));
    if (!ctx) {
        return NULL;
    }

    ctx->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epoll_fd < 0) {
        free(ctx);
        return NULL;
    }

    ctx->max_events = max_events > 0 ? max_events : kDefaultMaxEvents;
    ctx->events = calloc(ctx->max_events, sizeof(struct epoll_event));
    if (!ctx->events) {
        close(ctx->epoll_fd);
        free(ctx);
        return NULL;
    }

    return ctx;
}

static void reactor_destroy(void* context)
{
    if (!context) {
        return;
    }

    epoll_reactor_context_t* ctx = context;
    if (ctx->epoll_fd >= 0) {
        close(ctx->epoll_fd);
    }
    free(ctx->events);
    free(ctx);
}

static int reactor_register(void* context, galay_c_io_controller_t* controller, uint32_t events)
{
    if (!context || !controller) {
        return -EINVAL;
    }

    epoll_reactor_context_t* ctx = context;
    struct epoll_event ev = {0};
    ev.data.ptr = controller;
    ev.events = 0;

    if (events & GALAY_C_EVENT_READ) {
        ev.events |= EPOLLIN;
    }
    if (events & GALAY_C_EVENT_WRITE) {
        ev.events |= EPOLLOUT;
    }

    uint32_t registered = atomic_load_explicit(&controller->registered_events, memory_order_acquire);
    int op = (registered == GALAY_C_EVENT_NONE) ? EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    if (epoll_ctl(ctx->epoll_fd, op, controller->fd, &ev) < 0) {
        return -errno;
    }

    atomic_store_explicit(&controller->registered_events, events, memory_order_release);
    return 0;
}

static int reactor_unregister(void* context, galay_c_io_controller_t* controller)
{
    if (!context || !controller) {
        return -EINVAL;
    }

    epoll_reactor_context_t* ctx = context;

    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_DEL, controller->fd, NULL) < 0) {
        return -errno;
    }

    atomic_store_explicit(&controller->registered_events, GALAY_C_EVENT_NONE, memory_order_release);
    return 0;
}

static int reactor_wait(void* context,
                       galay_c_io_scheduler_t* scheduler,
                       ready_queue_t* ready_queue,
                       int timeout_ms)
{
    if (!context || !scheduler || !ready_queue) {
        return -EINVAL;
    }

    epoll_reactor_context_t* ctx = context;
    int n = epoll_wait(ctx->epoll_fd, ctx->events, ctx->max_events, timeout_ms);
    if (n < 0) {
        return -errno;
    }

    // 处理就绪事件
    for (int i = 0; i < n; i++) {
        galay_c_io_controller_t* controller = ctx->events[i].data.ptr;
        uint32_t revents = ctx->events[i].events;

        // 唤醒读协程
        if (revents & (EPOLLIN | EPOLLERR | EPOLLHUP)) {
            C_CoroTaskInternal* read_coro = atomic_exchange_explicit(&controller->read_slot,
                                                                      NULL,
                                                                      memory_order_acq_rel);
            if (read_coro) {
                atomic_store_explicit(&read_coro->wait_code, C_IOResultOk,
                                      memory_order_release);
                const C_IOResult wake_result = galay_c_coro_task_wake(read_coro);
                galay_c_coro_task_release(read_coro);
                if (wake_result.code == C_IOResultOk) {
                    atomic_fetch_add_explicit(&scheduler->wake_count, 1, memory_order_relaxed);
                }
            }
        }

        // 唤醒写协程
        if (revents & (EPOLLOUT | EPOLLERR | EPOLLHUP)) {
            C_CoroTaskInternal* write_coro = atomic_exchange_explicit(&controller->write_slot,
                                                                       NULL,
                                                                       memory_order_acq_rel);
            if (write_coro) {
                atomic_store_explicit(&write_coro->wait_code, C_IOResultOk,
                                      memory_order_release);
                const C_IOResult wake_result = galay_c_coro_task_wake(write_coro);
                galay_c_coro_task_release(write_coro);
                if (wake_result.code == C_IOResultOk) {
                    atomic_fetch_add_explicit(&scheduler->wake_count, 1, memory_order_relaxed);
                }
            }
        }
    }

    atomic_fetch_add_explicit(&scheduler->event_count, n, memory_order_relaxed);
    return n;
}

#elif defined(__APPLE__) || defined(__FreeBSD__)
// kqueue 实现类似，这里先占位
typedef struct kqueue_reactor_context {
    int kqueue_fd;
    struct kevent* events;
    int max_events;
} kqueue_reactor_context_t;

static void* reactor_create(int max_events)
{
    // TODO: implement kqueue version
    return NULL;
}

static void reactor_destroy(void* context) {}
static int reactor_register(void* context, galay_c_io_controller_t* controller, uint32_t events) { return -ENOSYS; }
static int reactor_unregister(void* context, galay_c_io_controller_t* controller) { return -ENOSYS; }
static int reactor_wait(void* context, galay_c_io_scheduler_t* scheduler, ready_queue_t* ready_queue, int timeout_ms) { return -ENOSYS; }
#endif

// Scheduler API 实现
C_IOResult galay_c_io_scheduler_create(galay_c_io_scheduler_t* out_scheduler,
                                       const galay_c_io_scheduler_options_t* options)
{
    if (!out_scheduler) {
        return make_result(C_IOResultInvalid, 0);
    }

    memset(out_scheduler, 0, sizeof(*out_scheduler));

    int max_events = options && options->max_events_per_wait > 0
        ? options->max_events_per_wait
        : kDefaultMaxEvents;

    void* reactor_ctx = reactor_create(max_events);
    if (!reactor_ctx) {
        return make_result(C_IOResultError, errno);
    }

    ready_queue_t* queue = ready_queue_create();
    if (!queue) {
        reactor_destroy(reactor_ctx);
        return make_result(C_IOResultError, errno);
    }

    out_scheduler->reactor_fd = 0; // 平台相关，暂时不暴露
    atomic_init(&out_scheduler->running, 0);
    atomic_init(&out_scheduler->active, 0);
    atomic_init(&out_scheduler->event_count, 0);
    atomic_init(&out_scheduler->wake_count, 0);
    atomic_init(&out_scheduler->reactor_epoch, 0);
    out_scheduler->ready_queue = queue;
    out_scheduler->reactor_context = reactor_ctx;

    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_io_scheduler_destroy(galay_c_io_scheduler_t* scheduler)
{
    if (!scheduler) {
        return make_result(C_IOResultInvalid, 0);
    }

    if (atomic_load_explicit(&scheduler->active, memory_order_acquire)) {
        return make_result(C_IOResultInvalid, 0);
    }

    reactor_destroy(scheduler->reactor_context);
    ready_queue_destroy(scheduler->ready_queue);

    memset(scheduler, 0, sizeof(*scheduler));
    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_io_scheduler_run(galay_c_io_scheduler_t* scheduler)
{
    if (!scheduler) {
        return make_result(C_IOResultInvalid, 0);
    }

    int expected = 0;
    if (!atomic_compare_exchange_strong_explicit(&scheduler->running,
                                                   &expected,
                                                   1,
                                                   memory_order_acq_rel,
                                                   memory_order_acquire)) {
        return make_result(C_IOResultInvalid, 0);
    }

    atomic_store_explicit(&scheduler->active, 1, memory_order_release);
    galay_c_current_scheduler = scheduler;

    ready_queue_t* queue = scheduler->ready_queue;
    C_CoroTaskInternal* batch[128];

    while (atomic_load_explicit(&scheduler->running, memory_order_acquire)) {
        galay_c_coro_task_process_timeouts(scheduler);

        // 1. 消费 ready queue，恢复所有就绪协程
        size_t ready_count = ready_queue_pop_batch(queue, batch, 128);
        for (size_t i = 0; i < ready_count; i++) {
            galay_c_coro_task_resume(batch[i]);
            galay_c_coro_task_release(batch[i]);
        }

        // 2. Reactor wait：等待 I/O 事件并唤醒协程
        const int timeout_ms = ready_queue_has_items(queue)
            ? 0
            : galay_c_coro_task_next_timeout_ms(scheduler, 10);
        int wait_result = reactor_wait(scheduler->reactor_context,
                                       scheduler,
                                       queue,
                                       timeout_ms);
        atomic_fetch_add_explicit(&scheduler->reactor_epoch, 1, memory_order_release);

        if (wait_result < 0 && wait_result != -EINTR) {
            atomic_store_explicit(&scheduler->running, 0, memory_order_release);
            atomic_store_explicit(&scheduler->active, 0, memory_order_release);
            galay_c_current_scheduler = NULL;
            return make_result(C_IOResultError, -wait_result);
        }
    }

    atomic_store_explicit(&scheduler->active, 0, memory_order_release);
    galay_c_current_scheduler = NULL;
    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_io_scheduler_stop(galay_c_io_scheduler_t* scheduler)
{
    if (!scheduler) {
        return make_result(C_IOResultInvalid, 0);
    }

    atomic_store_explicit(&scheduler->running, 0, memory_order_release);
    return make_result(C_IOResultOk, 0);
}

int galay_c_io_scheduler_is_running(const galay_c_io_scheduler_t* scheduler)
{
    if (!scheduler) {
        return 0;
    }

    return atomic_load_explicit(&scheduler->active, memory_order_acquire);
}

int galay_c_io_scheduler_is_current_thread(void)
{
    return galay_c_current_scheduler != NULL;
}

galay_c_io_scheduler_t* galay_c_io_scheduler_current(void)
{
    return galay_c_current_scheduler;
}

C_IOResult galay_c_io_scheduler_register(galay_c_io_scheduler_t* scheduler,
                                         galay_c_io_controller_t* controller,
                                         uint32_t events)
{
    if (!scheduler || !controller) {
        return make_result(C_IOResultInvalid, 0);
    }

    int result = reactor_register(scheduler->reactor_context, controller, events);
    if (result < 0) {
        return make_result(C_IOResultError, -result);
    }

    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_io_scheduler_unregister(galay_c_io_scheduler_t* scheduler,
                                           galay_c_io_controller_t* controller)
{
    if (!scheduler || !controller) {
        return make_result(C_IOResultInvalid, 0);
    }

    int result = reactor_unregister(scheduler->reactor_context, controller);
    if (result < 0) {
        return make_result(C_IOResultError, -result);
    }

    if (galay_c_current_scheduler != scheduler) {
        const uint64_t epoch = atomic_load_explicit(&scheduler->reactor_epoch,
                                                    memory_order_acquire);
        while (atomic_load_explicit(&scheduler->active, memory_order_acquire) &&
               atomic_load_explicit(&scheduler->reactor_epoch,
                                    memory_order_acquire) == epoch) {
            thrd_yield();
        }
    }

    return make_result(C_IOResultOk, 0);
}

C_IOResult galay_c_io_scheduler_modify(galay_c_io_scheduler_t* scheduler,
                                       galay_c_io_controller_t* controller,
                                       uint32_t events)
{
    // modify 和 register 实现相同（epoll_ctl 自动处理 MOD）
    return galay_c_io_scheduler_register(scheduler, controller, events);
}

C_IOResult galay_c_io_scheduler_enqueue_ready(galay_c_io_scheduler_t* scheduler,
                                              C_CoroTaskInternal* coro)
{
    if (!scheduler || !coro) {
        return make_result(C_IOResultInvalid, 0);
    }

    ready_queue_t* queue = scheduler->ready_queue;
    if (!ready_queue_push(queue, coro)) {
        return make_result(C_IOResultError, ENOMEM);
    }

    return make_result(C_IOResultOk, 0);
}
