#include "io_scheduler.h"
#include "../coro-c/coro_task_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#ifdef __linux__
#include <sys/epoll.h>
#include <sys/eventfd.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#include <sys/time.h>
#endif

#include <unistd.h>

// 简单的无锁 MPSC ready queue（多生产者单消费者）
typedef struct ready_queue_node {
    C_CoroTaskInternal* coro;
    _Atomic(struct ready_queue_node*) next;
} ready_queue_node_t;

typedef struct ready_queue {
    _Atomic(ready_queue_node_t*) head;   // producers push a private stack node
    ready_queue_node_t* pending;         // consumer-owned FIFO list
    ready_queue_node_t* cache;           // consumer/producer node cache
    size_t cache_count;
    atomic_flag cache_lock;              // cache only; never held across a push
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

    atomic_init(&queue->head, NULL);
    queue->pending = NULL;
    queue->cache = NULL;
    queue->cache_count = 0;
    atomic_flag_clear(&queue->cache_lock);

    return queue;
}

static void ready_queue_destroy(ready_queue_t* queue)
{
    if (!queue) {
        return;
    }

    ready_queue_node_t* node = atomic_exchange_explicit(&queue->head, NULL,
                                                         memory_order_acq_rel);
    while (node) {
        ready_queue_node_t* next = atomic_load_explicit(&node->next, memory_order_relaxed);
        free(node);
        node = next;
    }

    node = queue->pending;
    while (node) {
        ready_queue_node_t* next = atomic_load_explicit(&node->next, memory_order_relaxed);
        free(node);
        node = next;
    }

    node = queue->cache;
    while (node) {
        ready_queue_node_t* next = atomic_load_explicit(&node->next, memory_order_relaxed);
        free(node);
        node = next;
    }

    free(queue);
}

static ready_queue_node_t* ready_queue_acquire_node(ready_queue_t* queue)
{
    ready_queue_node_t* node = NULL;
    if (atomic_flag_test_and_set_explicit(&queue->cache_lock, memory_order_acquire) == 0) {
        if (queue->cache != NULL) {
            node = queue->cache;
            queue->cache = atomic_load_explicit(&node->next, memory_order_relaxed);
            --queue->cache_count;
        }
        atomic_flag_clear_explicit(&queue->cache_lock, memory_order_release);
    }
    return node != NULL ? node : calloc(1, sizeof(ready_queue_node_t));
}

static void ready_queue_release_node(ready_queue_t* queue, ready_queue_node_t* node)
{
    if (queue == NULL || node == NULL) {
        return;
    }
    if (atomic_flag_test_and_set_explicit(&queue->cache_lock, memory_order_acquire) == 0) {
        if (queue->cache_count < kReadyQueueCacheSize) {
            atomic_store_explicit(&node->next, queue->cache, memory_order_relaxed);
            queue->cache = node;
            ++queue->cache_count;
            node = NULL;
        }
        atomic_flag_clear_explicit(&queue->cache_lock, memory_order_release);
    }
    free(node);
}

// Producers publish a private LIFO node. The consumer exchanges the whole
// stack and reverses it, so producers never write through a node that the
// consumer may be reclaiming.
static int ready_queue_push(ready_queue_t* queue, C_CoroTaskInternal* coro)
{
    if (!queue || !coro) {
        return 0;
    }

    ready_queue_node_t* node = ready_queue_acquire_node(queue);
    if (node == NULL) {
        return 0;
    }
    node->coro = coro;
    ready_queue_node_t* head = atomic_load_explicit(&queue->head, memory_order_relaxed);
    do {
        atomic_store_explicit(&node->next, head, memory_order_relaxed);
    } while (!atomic_compare_exchange_weak_explicit(&queue->head,
                                                     &head,
                                                     node,
                                                     memory_order_release,
                                                     memory_order_relaxed));
    return 1;
}

static void ready_queue_refill_pending(ready_queue_t* queue)
{
    ready_queue_node_t* stack = atomic_exchange_explicit(&queue->head, NULL,
                                                         memory_order_acquire);
    while (stack != NULL) {
        ready_queue_node_t* next = atomic_load_explicit(&stack->next, memory_order_relaxed);
        atomic_store_explicit(&stack->next, queue->pending, memory_order_relaxed);
        queue->pending = stack;
        stack = next;
    }
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
    while (count < max_count) {
        if (queue->pending == NULL) {
            ready_queue_refill_pending(queue);
            if (queue->pending == NULL) {
                break;
            }
        }

        ready_queue_node_t* node = queue->pending;
        queue->pending = atomic_load_explicit(&node->next, memory_order_relaxed);
        out_coros[count++] = node->coro;
        ready_queue_release_node(queue, node);
    }
    return count;
}

static int ready_queue_has_items(const ready_queue_t* queue)
{
    return queue != NULL &&
           (queue->pending != NULL ||
            atomic_load_explicit(&queue->head, memory_order_acquire) != NULL);
}

// Reactor context
#ifdef __linux__
typedef struct epoll_reactor_context {
    int epoll_fd;
    int wake_fd;  // eventfd：跨线程入队时唤醒 epoll_wait
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

    ctx->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ctx->wake_fd < 0) {
        close(ctx->epoll_fd);
        free(ctx);
        return NULL;
    }

    // 水平触发的唤醒 fd：计数器非零时 epoll_wait 立即返回，杜绝丢失唤醒。
    struct epoll_event wake_ev = {0};
    wake_ev.events = EPOLLIN;
    wake_ev.data.ptr = NULL;
    if (epoll_ctl(ctx->epoll_fd, EPOLL_CTL_ADD, ctx->wake_fd, &wake_ev) < 0) {
        close(ctx->wake_fd);
        close(ctx->epoll_fd);
        free(ctx);
        return NULL;
    }

    ctx->max_events = max_events > 0 ? max_events : kDefaultMaxEvents;
    ctx->events = calloc(ctx->max_events, sizeof(struct epoll_event));
    if (!ctx->events) {
        close(ctx->wake_fd);
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
    if (ctx->wake_fd >= 0) {
        close(ctx->wake_fd);
    }
    if (ctx->epoll_fd >= 0) {
        close(ctx->epoll_fd);
    }
    free(ctx->events);
    free(ctx);
}

// 唤醒阻塞在 epoll_wait 的事件循环；EAGAIN 说明计数器已非零，唤醒必然发生。
static void reactor_notify(void* context)
{
    epoll_reactor_context_t* ctx = context;
    if (ctx == NULL || ctx->wake_fd < 0) {
        return;
    }
    const uint64_t one = 1;
    (void)write(ctx->wake_fd, &one, sizeof(one));
}

static int reactor_register(void* context, galay_c_io_controller_t* controller, uint32_t events)
{
    if (!context || !controller) {
        return -EINVAL;
    }

    epoll_reactor_context_t* ctx = context;
    struct epoll_event ev = {0};
    ev.data.ptr = controller;
    ev.events = EPOLLET;

    if (events & GALAY_C_EVENT_READ) {
        ev.events |= EPOLLIN;
    }
    if (events & GALAY_C_EVENT_WRITE) {
        ev.events |= EPOLLOUT;
    }

    uint32_t registered = atomic_load_explicit(&controller->registered_events, memory_order_acquire);
    if (registered == events) {
        return 0;
    }
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
    int io_events = 0;
    for (int i = 0; i < n; i++) {
        galay_c_io_controller_t* controller = ctx->events[i].data.ptr;
        if (controller == NULL) {
            // 跨线程入队唤醒：排空 eventfd 后继续处理其余 I/O 事件。
            uint64_t wake_value = 0;
            while (read(ctx->wake_fd, &wake_value, sizeof(wake_value)) > 0) {}
            continue;
        }
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
        ++io_events;
    }

    atomic_fetch_add_explicit(&scheduler->event_count, io_events, memory_order_relaxed);
    return io_events;
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
static void reactor_notify(void* context) {}
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
    atomic_init(&out_scheduler->reactor_inflight, 0);
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
        // unregister() 使用该计数等待本批次完成。计数覆盖整个
        // reactor_wait，而不是只覆盖 epoll_wait，确保 controller 指针在
        // 事件处理完成前始终由调用方持有。
        atomic_fetch_add_explicit(&scheduler->reactor_inflight, 1, memory_order_acq_rel);
        int wait_result = reactor_wait(scheduler->reactor_context,
                                       scheduler,
                                       queue,
                                       timeout_ms);
        atomic_fetch_sub_explicit(&scheduler->reactor_inflight, 1, memory_order_release);
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

    const int result = reactor_unregister(scheduler->reactor_context, controller);
    if (galay_c_current_scheduler != scheduler) {
        // epoll_wait/kevent may already have copied controller into the current
        // event batch when DEL returns. Wait for the complete batch, including
        // all slot exchanges and task wakeups, before the owner can free it.
        while (atomic_load_explicit(&scheduler->reactor_inflight,
                                    memory_order_acquire) != 0) {
            thrd_yield();
        }
    }

    if (result < 0) {
        return make_result(C_IOResultError, -result);
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

    // 跨线程入队必须唤醒 reactor；否则事件循环会在 epoll_wait 里最多空等一个
    // poll 周期（默认 10ms）。调度器线程自身入队（yield 重排）无需唤醒。
    if (galay_c_current_scheduler != scheduler) {
        reactor_notify(scheduler->reactor_context);
    }

    return make_result(C_IOResultOk, 0);
}
