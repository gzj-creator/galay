#ifndef GALAY_KERNEL_CORE_C_CORO_FILE_WATCHER_BRIDGE_H
#define GALAY_KERNEL_CORE_C_CORO_FILE_WATCHER_BRIDGE_H

#include "c_coro_tcp_bridge.h"

#include <stdbool.h>

/**
 * @file c_coro_file_watcher_bridge.h
 * @brief C++ kernel core 暴露的 C 协程 FileWatcher adapter。
 *
 * @details 该内部 C 风格桥接把 FileWatchAwaitable/IOController 细节限制在
 * kernel core 内，C ABI wrapper 只负责 C coroutine wait token 和结果码转换。
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum GalayCoreCoroFileWatchEvent {
    GalayCoreCoroFileWatchEventNone = 0,
    GalayCoreCoroFileWatchEventAccess = 0x00000001,
    GalayCoreCoroFileWatchEventModify = 0x00000002,
    GalayCoreCoroFileWatchEventAttrib = 0x00000004,
    GalayCoreCoroFileWatchEventCloseWrite = 0x00000008,
    GalayCoreCoroFileWatchEventCloseNoWrite = 0x00000010,
    GalayCoreCoroFileWatchEventOpen = 0x00000020,
    GalayCoreCoroFileWatchEventMovedFrom = 0x00000040,
    GalayCoreCoroFileWatchEventMovedTo = 0x00000080,
    GalayCoreCoroFileWatchEventCreate = 0x00000100,
    GalayCoreCoroFileWatchEventDelete = 0x00000200,
    GalayCoreCoroFileWatchEventDeleteSelf = 0x00000400,
    GalayCoreCoroFileWatchEventMoveSelf = 0x00000800,
    GalayCoreCoroFileWatchEventAll = 0x00000FFF,
} GalayCoreCoroFileWatchEvent;

typedef struct GalayCoreCoroFileWatchResult {
    GalayCoreCoroFileWatchEvent events;
    char name[256];
    bool is_dir;
} GalayCoreCoroFileWatchResult;

GalayCoreCoroIOResult galay_core_coro_file_watcher_watch(
    void* watcher,
    void* scheduler,
    GalayCoreCoroFileWatchResult* out_result,
    int64_t timeout_ms,
    void* user_data,
    const GalayCoreCoroWaitOps* wait_ops);

#ifdef __cplusplus
}
#endif

#endif
