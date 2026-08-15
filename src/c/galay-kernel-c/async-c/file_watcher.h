#ifndef GALAY_C_KERNEL_ASYNC_FILE_WATCHER_H
#define GALAY_C_KERNEL_ASYNC_FILE_WATCHER_H

/**
 * @file file_watcher.h
 * @brief 原生 C 文件监听，使用 inotify (Linux) 或 kqueue (macOS)。
 *
 * @details 提供高性能的文件系统事件监听：
 * - inotify (Linux)
 * - kqueue (macOS/BSD)
 * - 与 native scheduler 集成
 */

#include <galay/c/galay-kernel-c/core-c/io_controller.h>
#include <galay/c/galay-kernel-c/core-c/io_scheduler.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 文件监听事件类型。
 */
typedef enum galay_c_watch_event {
    GALAY_C_WATCH_CREATE = 0x01,       // 文件创建
    GALAY_C_WATCH_DELETE = 0x02,       // 文件删除
    GALAY_C_WATCH_MODIFY = 0x04,       // 文件修改
    GALAY_C_WATCH_MOVE = 0x08,         // 文件移动
    GALAY_C_WATCH_ATTRIB = 0x10,       // 属性变化
    GALAY_C_WATCH_ALL = 0x1F,          // 所有事件
} galay_c_watch_event_t;

/**
 * @brief 文件监听事件。
 */
typedef struct galay_c_file_event {
    uint32_t mask;                     // 事件类型掩码
    uint32_t cookie;                   // 关联的 cookie（用于 MOVE）
    char name[256];                    // 文件名（相对于监听目录）
    int is_dir;                        // 是否是目录
} galay_c_file_event_t;

/**
 * @brief 原生 C 文件监听器。
 */
typedef struct galay_c_file_watcher_native {
    int fd;                            // inotify fd 或 kqueue fd
    galay_c_io_controller_t controller;
    galay_c_io_scheduler_t* scheduler;
    void* watch_map;                   // 内部：wd -> path 映射
} galay_c_file_watcher_t;

/**
 * @brief 创建文件监听器。
 *
 * @param out_watcher 输出监听器结构。
 * @return 成功返回 C_IOResultOk；失败返回对应错误码。
 */
C_IOResult galay_c_file_watcher_create(galay_c_file_watcher_t* out_watcher);

/**
 * @brief 添加监听路径。
 *
 * @param watcher 文件监听器。
 * @param path 待监听的文件或目录路径。
 * @param events 监听的事件类型掩码（GALAY_C_WATCH_* 组合）。
 * @return 成功返回 C_IOResultOk 且 value 字段为 watch descriptor；失败返回对应错误码。
 *
 * @note 返回的 watch descriptor 可用于后续移除监听。
 */
C_IOResult galay_c_file_watcher_add_watch(galay_c_file_watcher_t* watcher,
                                                 const char* path,
                                                 uint32_t events);

/**
 * @brief 移除监听路径。
 *
 * @param watcher 文件监听器。
 * @param wd watch descriptor（由 add_watch 返回）。
 * @return 成功返回 C_IOResultOk；失败返回对应错误码。
 */
C_IOResult galay_c_file_watcher_remove_watch(galay_c_file_watcher_t* watcher,
                                                    int wd);

/**
 * @brief 等待文件事件（协程挂起直到有事件或超时）。
 *
 * @param watcher 文件监听器。
 * @param out_event 输出事件结构（最多返回一个事件）。
 * @param timeout_ms 超时毫秒数；负数表示无限等待。
 * @return 成功返回 C_IOResultOk；超时返回 C_IOResultTimeout；错误返回 C_IOResultError/errno。
 *
 * @note 该函数每次返回一个事件。如果有多个事件，需要多次调用。
 */
C_IOResult galay_c_file_watcher_wait(galay_c_file_watcher_t* watcher,
                                            galay_c_file_event_t* out_event,
                                            int64_t timeout_ms);

/**
 * @brief 关闭文件监听器。
 *
 * @param watcher 文件监听器。
 * @return 成功返回 C_IOResultOk；失败返回对应错误码。
 *
 * @note 会自动移除所有监听路径。
 */
C_IOResult galay_c_file_watcher_close(galay_c_file_watcher_t* watcher);

#ifdef __cplusplus
}
#endif

#endif
