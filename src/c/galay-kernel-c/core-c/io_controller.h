#ifndef GALAY_C_KERNEL_CORE_IO_CONTROLLER_H
#define GALAY_C_KERNEL_CORE_IO_CONTROLLER_H

/**
 * @file io_controller.h
 * @brief 原生 C IO Controller，直接对接 OS reactor，无 C++ bridge 开销。
 *
 * @details 为 C 协程提供零开销的 I/O 多路复用。每个 fd 对应一个 controller，
 * 使用原子操作管理读写协程注册，避免锁和动态分配。
 */

#include <galay/c/galay-common-c/common/galay_c_defs.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result.h>

#include <stdint.h>

#include <stdatomic.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct galay_c_io_scheduler galay_c_io_scheduler_t;
typedef struct C_CoroTaskInternal C_CoroTaskInternal;

/**
 * @brief I/O 事件类型。
 */
typedef enum galay_c_io_event {
    GALAY_C_EVENT_NONE = 0,
    GALAY_C_EVENT_READ = 1,
    GALAY_C_EVENT_WRITE = 2,
    GALAY_C_EVENT_ERROR = 4,
    GALAY_C_EVENT_HUP = 8,
} galay_c_io_event_t;

/**
 * @brief 原生 C IO Controller。
 *
 * @details 管理单个 fd 的 I/O 事件和协程注册。使用无锁原子操作：
 * - read_slot/write_slot：注册的协程指针，NULL 表示空闲
 * - registered_events：当前在 reactor 中注册的事件掩码
 * - owner_scheduler：拥有该 controller 的 scheduler
 *
 * 生命周期：
 * - 由 socket/file 对象持有
 * - close 时必须确保无挂起协程
 */
typedef struct galay_c_io_controller {
    int fd;

    // 读写槽位：注册等待该事件的协程（NULL = 空闲）
    GALAY_C_ATOMIC(C_CoroTaskInternal*) read_slot;
    GALAY_C_ATOMIC(C_CoroTaskInternal*) write_slot;

    // 当前在 reactor 中注册的事件掩码
    GALAY_C_ATOMIC(uint32_t) registered_events;

    // 拥有该 controller 的 scheduler（用于验证跨 scheduler 操作）
    GALAY_C_ATOMIC(galay_c_io_scheduler_t*) owner_scheduler;

    // 用户数据（socket/file 对象指针）
    void* user_data;
} galay_c_io_controller_t;

/**
 * @brief 初始化 IO controller。
 *
 * @param controller 输出 controller，必须是未初始化的内存。
 * @param fd 文件描述符，必须非负且已设置为非阻塞模式。
 * @param user_data 可选用户数据指针。
 * @return 成功返回 C_IOResultOk；参数无效返回 C_IOResultInvalid。
 */
C_IOResult galay_c_io_controller_init(galay_c_io_controller_t* controller,
                                      int fd,
                                      void* user_data);

/**
 * @brief 清理 IO controller。
 *
 * @param controller 待清理的 controller。
 * @return 成功返回 C_IOResultOk；仍有挂起协程返回 C_IOResultInvalid。
 *
 * @note 调用前必须确保没有协程在等待该 controller 的 I/O 事件。
 */
C_IOResult galay_c_io_controller_cleanup(galay_c_io_controller_t* controller);

/**
 * @brief 原子注册当前协程到读槽位。
 *
 * @param controller IO controller。
 * @param coro 当前协程任务，必须非 NULL。
 * @param scheduler 当前 scheduler，用于验证所有权。
 * @return 成功返回 C_IOResultOk；槽位已占用或 scheduler 不匹配返回 C_IOResultInvalid。
 *
 * @note 该函数仅设置槽位，不修改 reactor 注册。调用后需调用
 * galay_c_io_controller_update_reactor 更新 reactor。
 */
C_IOResult galay_c_io_controller_register_read(galay_c_io_controller_t* controller,
                                                C_CoroTaskInternal* coro,
                                                galay_c_io_scheduler_t* scheduler);

/**
 * @brief 原子注册当前协程到写槽位。
 *
 * @param controller IO controller。
 * @param coro 当前协程任务，必须非 NULL。
 * @param scheduler 当前 scheduler，用于验证所有权。
 * @return 成功返回 C_IOResultOk；槽位已占用或 scheduler 不匹配返回 C_IOResultInvalid。
 */
C_IOResult galay_c_io_controller_register_write(galay_c_io_controller_t* controller,
                                                 C_CoroTaskInternal* coro,
                                                 galay_c_io_scheduler_t* scheduler);

/**
 * @brief 原子清除读槽位。
 *
 * @param controller IO controller。
 * @param expected_coro 期望的协程指针，只有匹配时才清除（NULL 表示无条件清除）。
 * @return 成功清除返回 C_IOResultOk；槽位为空或不匹配返回 C_IOResultInvalid。
 */
C_IOResult galay_c_io_controller_clear_read(galay_c_io_controller_t* controller,
                                             C_CoroTaskInternal* expected_coro);

/**
 * @brief 原子清除写槽位。
 *
 * @param controller IO controller。
 * @param expected_coro 期望的协程指针，只有匹配时才清除（NULL 表示无条件清除）。
 * @return 成功清除返回 C_IOResultOk；槽位为空或不匹配返回 C_IOResultInvalid。
 */
C_IOResult galay_c_io_controller_clear_write(galay_c_io_controller_t* controller,
                                              C_CoroTaskInternal* expected_coro);

/**
 * @brief 检查读槽位是否被指定协程占用。
 *
 * @param controller IO controller。
 * @param coro 待检查的协程指针。
 * @return 槽位被该协程占用返回非 0；否则返回 0。
 */
int galay_c_io_controller_has_read_coro(const galay_c_io_controller_t* controller,
                                        const C_CoroTaskInternal* coro);

/**
 * @brief 检查写槽位是否被指定协程占用。
 *
 * @param controller IO controller。
 * @param coro 待检查的协程指针。
 * @return 槽位被该协程占用返回非 0；否则返回 0。
 */
int galay_c_io_controller_has_write_coro(const galay_c_io_controller_t* controller,
                                         const C_CoroTaskInternal* coro);

#ifdef __cplusplus
}
#endif

#endif
