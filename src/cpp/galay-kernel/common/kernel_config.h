/**
 * @file kernel_config.h
 * @brief galay-kernel 运行时可调参数宏集中定义
 * @author galay-kernel
 *
 * @details 集中存放影响运行时行为的数值型调优宏，统一在此调整，
 *          避免散落在各实现文件中。均带 #ifndef 保护，允许编译期外部覆盖。
 */

#ifndef GALAY_KERNEL_CONFIG_H
#define GALAY_KERNEL_CONFIG_H

/**
 * @def GALAY_KERNEL_TIMER_WHEEL_TICK_NS
 * @brief IOScheduler 时间轮默认 tick 粒度（纳秒）
 *
 * 决定 IO 超时（.timeout()）的触发精度上界：定时器在到期后的下一个
 * tick 边界触发，延迟不超过一个 tick。保持历史 50ms 默认值，避免在
 * 没有基准证据时增加空闲事件循环唤醒频率。
 */
#ifndef GALAY_KERNEL_TIMER_WHEEL_TICK_NS
#define GALAY_KERNEL_TIMER_WHEEL_TICK_NS 50000000ULL
#endif

/**
 * @def GALAY_KERNEL_IO_POLL_IDLE_TIMEOUT_MS
 * @brief 时间轮为空时事件循环 poll 的空闲阻塞上限（毫秒）
 *
 * epoll/kqueue 使用该值作为空轮等待上限；io_uring 还会与
 * GALAY_KERNEL_IO_POLL_WAIT_MAX_NS 取最小值。
 */
#ifndef GALAY_KERNEL_IO_POLL_IDLE_TIMEOUT_MS
#define GALAY_KERNEL_IO_POLL_IDLE_TIMEOUT_MS 50
#endif

/**
 * @def GALAY_KERNEL_IO_POLL_TIMEOUT_MIN_MS
 * @brief 非空轮 poll 超时下限（毫秒）
 */
#ifndef GALAY_KERNEL_IO_POLL_TIMEOUT_MIN_MS
#define GALAY_KERNEL_IO_POLL_TIMEOUT_MIN_MS 1
#endif

/**
 * @def GALAY_KERNEL_IO_POLL_TIMEOUT_MAX_MS
 * @brief 非空轮 poll 超时上限（毫秒）
 */
#ifndef GALAY_KERNEL_IO_POLL_TIMEOUT_MAX_MS
#define GALAY_KERNEL_IO_POLL_TIMEOUT_MAX_MS 1000
#endif

/**
 * @def GALAY_KERNEL_IO_POLL_WAIT_MAX_NS
 * @brief io_uring 完成等待的绝对上限（纳秒）。
 *
 * 它独立于毫秒后端的 idle/timeout 配置；io_uring 最终等待时间取时间轮
 * 推导值与本上限的较小者。毫秒后端使用 GALAY_KERNEL_IO_POLL_TIMEOUT_MAX_MS，
 * 并在边界处向上取整。
 * 旧的 GALAY_IOURING_WAIT_TIMEOUT_NS 名称仅作为同值兼容别名保留。
 */
#ifndef GALAY_KERNEL_IO_POLL_WAIT_MAX_NS
#ifdef GALAY_IOURING_WAIT_TIMEOUT_NS
#define GALAY_KERNEL_IO_POLL_WAIT_MAX_NS GALAY_IOURING_WAIT_TIMEOUT_NS
#else
#define GALAY_KERNEL_IO_POLL_WAIT_MAX_NS 10000000ULL
#endif
#endif

// 配置优先级：调用方显式提供的新宏优先；仅提供旧宏时由它初始化上面的
// 新宏；两者都未提供时，再把旧名称定义为指向新宏的单向别名，避免两个
// 名称同时出现在构建参数中时发生递归展开。
#ifndef GALAY_IOURING_WAIT_TIMEOUT_NS
#define GALAY_IOURING_WAIT_TIMEOUT_NS GALAY_KERNEL_IO_POLL_WAIT_MAX_NS
#endif

/**
 * @def GALAY_SCHEDULER_MAX_EVENTS
 * @brief 调度器单次 poll 处理的最大事件数
 */
#ifndef GALAY_SCHEDULER_MAX_EVENTS
#define GALAY_SCHEDULER_MAX_EVENTS 1024
#endif

/**
 * @def GALAY_SCHEDULER_BATCH_SIZE
 * @brief 调度器单批恢复的协程批量大小
 */
#ifndef GALAY_SCHEDULER_BATCH_SIZE
#define GALAY_SCHEDULER_BATCH_SIZE 256
#endif

/**
 * @def GALAY_SCHEDULER_QUEUE_DEPTH
 * @brief io_uring 提交/完成队列深度
 */
#ifndef GALAY_SCHEDULER_QUEUE_DEPTH
#define GALAY_SCHEDULER_QUEUE_DEPTH 4096
#endif

/**
 * @def GALAY_KERNEL_TIMEOUT_TIMER_POOL_MAX_CACHED
 * @brief 每个调度线程本地 TimeoutTimer 池最多缓存的对象数
 */
#ifndef GALAY_KERNEL_TIMEOUT_TIMER_POOL_MAX_CACHED
#define GALAY_KERNEL_TIMEOUT_TIMER_POOL_MAX_CACHED 256
#endif

#endif  // GALAY_KERNEL_CONFIG_H
