#ifndef GALAY_C_KERNEL_CORE_IO_SCHEDULER_H
#define GALAY_C_KERNEL_CORE_IO_SCHEDULER_H

/**
 * @file io_scheduler.h
 * @brief 原生 C IO Scheduler，直接管理 reactor 和协程调度，无 C++ bridge。
 *
 * @details 为 C 协程提供高性能 I/O 事件循环：
 * - 直接封装 epoll/kqueue/io_uring，无 C++ 抽象层
 * - Lock-free ready queue，协程完成后立即入队
 * - 零动态分配的热路径（controller 和 coro 预分配）
 *
 * 架构：
 *   C Coroutine → IO Controller → Native Scheduler → Reactor (epoll/kqueue)
 */

#include "io_controller.h"
#include <galay/c/galay-kernel-c/coro-c/coro_result.h>

#include <stddef.h>
#include <stdint.h>

#include <stdatomic.h>
#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 原生 C IO Scheduler。
 *
 * @details 管理一个线程的 I/O 事件循环和协程调度：
 * - reactor_fd：epoll_fd (Linux) 或 kqueue_fd (macOS)
 * - ready_queue：完成的协程队列，由 reactor 填充，scheduler loop 消费
 * - running：运行状态标志
 */
typedef struct galay_c_io_scheduler {
    int reactor_fd;                          // epoll/kqueue fd
    GALAY_C_ATOMIC(int) running;              // 0=stopped, 1=running
    GALAY_C_ATOMIC(int) active;               // scheduler loop thread is active
    GALAY_C_ATOMIC(uint64_t) event_count;     // 处理的事件总数（统计）
    GALAY_C_ATOMIC(uint64_t) wake_count;      // 唤醒的协程总数（统计）
    GALAY_C_ATOMIC(uint64_t) reactor_epoch;   // completed reactor batches
    GALAY_C_ATOMIC(uint32_t) reactor_inflight; // 正在读取 event.data.ptr 的 reactor 批次
    void* ready_queue;                       // 内部 ready queue 实现
    void* reactor_context;                   // 平台相关 reactor 上下文
    void* timeout_head;                      // scheduler 线程私有 deadline 链表
} galay_c_io_scheduler_t;

/**
 * @brief Scheduler 创建选项。
 */
typedef struct galay_c_io_scheduler_options {
    int max_events_per_wait;                 // 单次 wait 最多返回事件数（0=默认128）
} galay_c_io_scheduler_options_t;

/**
 * @brief 创建原生 C IO Scheduler。
 *
 * @param out_scheduler 输出 scheduler，必须是未初始化的内存。
 * @param options 可选创建选项，NULL 表示使用默认值。
 * @return 成功返回 C_IOResultOk；参数无效返回 C_IOResultInvalid；
 * 系统资源不足返回 C_IOResultError/errno。
 *
 * @note 创建后 scheduler 处于 stopped 状态，需调用 galay_c_io_scheduler_run 启动。
 */
C_IOResult galay_c_io_scheduler_create(galay_c_io_scheduler_t* out_scheduler,
                                       const galay_c_io_scheduler_options_t* options);

/**
 * @brief 销毁原生 C IO Scheduler。
 *
 * @param scheduler 待销毁的 scheduler。
 * @return 成功返回 C_IOResultOk；scheduler 仍在运行返回 C_IOResultInvalid。
 *
 * @note 必须先调用 galay_c_io_scheduler_stop 并等待 run 返回。
 */
C_IOResult galay_c_io_scheduler_destroy(galay_c_io_scheduler_t* scheduler);

/**
 * @brief 在当前线程上运行 scheduler 事件循环。
 *
 * @param scheduler IO scheduler。
 * @return 正常停止返回 C_IOResultOk；参数无效或已在运行返回 C_IOResultInvalid；
 * reactor 错误返回 C_IOResultError。
 *
 * @note 该函数会阻塞直到 galay_c_io_scheduler_stop 被调用。
 * 事件循环：
 *   1. 从 ready_queue 消费并恢复所有就绪协程
 *   2. reactor wait（epoll_wait/kevent）等待 I/O 事件
 *   3. 将完成的协程入队到 ready_queue
 *   4. 重复
 */
C_IOResult galay_c_io_scheduler_run(galay_c_io_scheduler_t* scheduler);

/**
 * @brief 停止 scheduler 事件循环。
 *
 * @param scheduler IO scheduler。
 * @return 成功返回 C_IOResultOk；参数无效返回 C_IOResultInvalid。
 *
 * @note 该函数是线程安全的，可从任意线程调用。run 函数会在下一次循环迭代后返回。
 */
C_IOResult galay_c_io_scheduler_stop(galay_c_io_scheduler_t* scheduler);

/**
 * @brief 检查 scheduler 是否正在运行。
 *
 * @param scheduler IO scheduler。
 * @return 正在运行返回非 0；否则返回 0。
 */
int galay_c_io_scheduler_is_running(const galay_c_io_scheduler_t* scheduler);

/**
 * @brief 返回调用线程是否正在执行 C scheduler loop。
 */
int galay_c_io_scheduler_is_current_thread(void);

/**
 * @brief 获取当前线程正在执行的 C scheduler。
 * @return scheduler loop 线程内返回借用指针；其它线程返回 NULL。
 * @note 返回值不转移所有权，仅在对应 runtime/scheduler 存活期间有效。
 */
galay_c_io_scheduler_t* galay_c_io_scheduler_current(void);

/**
 * @brief 注册 controller 到 reactor。
 *
 * @param scheduler IO scheduler。
 * @param controller IO controller，必须已初始化且 fd 有效。
 * @param events 待注册的事件掩码（GALAY_C_EVENT_READ | GALAY_C_EVENT_WRITE）。
 * @return 成功返回 C_IOResultOk；参数无效或 reactor 错误返回对应错误码。
 *
 * @note 该函数更新 controller->registered_events 并调用 epoll_ctl/kevent。
 * 如果 controller 已注册，会执行 MOD 操作；否则执行 ADD 操作。
 */
C_IOResult galay_c_io_scheduler_register(galay_c_io_scheduler_t* scheduler,
                                         galay_c_io_controller_t* controller,
                                         uint32_t events);

/**
 * @brief 从 reactor 注销 controller。
 *
 * @param scheduler IO scheduler。
 * @param controller IO controller。
 * @return 成功返回 C_IOResultOk；参数无效或 reactor 错误返回对应错误码。
 *
 * @note 该函数会清除 controller->registered_events 并调用 epoll_ctl(DEL)/kevent。
 * 从非 scheduler 线程调用时，会等待当前 reactor 批次退出后再返回，调用方随后
 * 可以安全释放包含 controller 的对象；scheduler 线程内调用不会阻塞。
 */
C_IOResult galay_c_io_scheduler_unregister(galay_c_io_scheduler_t* scheduler,
                                           galay_c_io_controller_t* controller);

/**
 * @brief 修改已注册 controller 的事件掩码。
 *
 * @param scheduler IO scheduler。
 * @param controller IO controller。
 * @param events 新的事件掩码。
 * @return 成功返回 C_IOResultOk；参数无效或 reactor 错误返回对应错误码。
 */
C_IOResult galay_c_io_scheduler_modify(galay_c_io_scheduler_t* scheduler,
                                       galay_c_io_controller_t* controller,
                                       uint32_t events);

/**
 * @brief 手动将协程入队到 ready queue（用于 yield/timeout 等非 I/O 唤醒）。
 *
 * @param scheduler IO scheduler。
 * @param coro 待入队的协程任务。
 * @return 成功返回 C_IOResultOk；参数无效或 queue 满返回 C_IOResultInvalid。
 *
 * @note 该函数是线程安全的，可从任意线程调用（例如 timer 线程）。
 */
C_IOResult galay_c_io_scheduler_enqueue_ready(galay_c_io_scheduler_t* scheduler,
                                              C_CoroTaskInternal* coro);

#ifdef __cplusplus
}
#endif

#endif
