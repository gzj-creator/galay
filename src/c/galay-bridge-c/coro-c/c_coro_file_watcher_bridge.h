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

/**
 * @brief FileWatcher 事件位掩码。
 *
 * @details 事件值与底层文件监听事件语义对应，可按位组合。All 表示当前 bridge
 * 暴露的全部事件位。
 */
typedef enum GalayCoreCoroFileWatchEvent {
    GalayCoreCoroFileWatchEventNone = 0,              ///< 无事件。
    GalayCoreCoroFileWatchEventAccess = 0x00000001,  ///< 文件被访问。
    GalayCoreCoroFileWatchEventModify = 0x00000002,  ///< 文件内容被修改。
    GalayCoreCoroFileWatchEventAttrib = 0x00000004,  ///< 文件属性被修改。
    GalayCoreCoroFileWatchEventCloseWrite = 0x00000008,    ///< 写入后关闭。
    GalayCoreCoroFileWatchEventCloseNoWrite = 0x00000010,  ///< 未写入关闭。
    GalayCoreCoroFileWatchEventOpen = 0x00000020,          ///< 文件被打开。
    GalayCoreCoroFileWatchEventMovedFrom = 0x00000040,     ///< 文件从监听路径移出。
    GalayCoreCoroFileWatchEventMovedTo = 0x00000080,       ///< 文件移入监听路径。
    GalayCoreCoroFileWatchEventCreate = 0x00000100,        ///< 文件或目录被创建。
    GalayCoreCoroFileWatchEventDelete = 0x00000200,        ///< 文件或目录被删除。
    GalayCoreCoroFileWatchEventDeleteSelf = 0x00000400,    ///< 被监听对象自身被删除。
    GalayCoreCoroFileWatchEventMoveSelf = 0x00000800,      ///< 被监听对象自身被移动。
    GalayCoreCoroFileWatchEventAll = 0x00000FFF,           ///< 当前暴露的全部事件位。
} GalayCoreCoroFileWatchEvent;

/**
 * @brief FileWatcher 单次事件结果。
 *
 * @details name 为内联固定长度缓存，成功返回时包含以 '\0' 结尾的相对名称或空串。
 * out_result 由调用方提供并拥有。
 */
typedef struct GalayCoreCoroFileWatchResult {
    GalayCoreCoroFileWatchEvent events;  ///< 触发的事件位。
    char name[256];                      ///< 事件名称缓存，以 '\0' 结尾。
    bool is_dir;                         ///< true 表示事件目标为目录。
} GalayCoreCoroFileWatchResult;

/**
 * @brief 提交 FileWatcher watch awaitable 并等待一个事件。
 *
 * @param watcher 内部 FileWatcher 指针，必须非 NULL。
 * @param scheduler 内部 IOScheduler 指针，必须属于当前 C coroutine。
 * @param out_result 输出事件结果，必须非 NULL。
 * @param timeout_ms 负数无限等待，0 立即返回 Timeout，正数为毫秒超时。
 * @param user_data runtime token，必须非 NULL，由 wait_ops 管理完成和释放。
 * @param wait_ops runtime 等待/完成回调表，所有回调必须非 NULL。
 * @return 成功返回 Ok，并写入 out_result；超时返回 Timeout；参数、scheduler、
 * wait hook 或 watcher 状态无效返回 Invalid；底层监听错误返回 Error/sys_errno。
 *
 * @note out_result 和 user_data 必须在函数返回或清理完成前保持有效。wait 回调应
 * 挂起 coroutine，不应阻塞 scheduler 线程。
 */
GalayCoreCoroIOResult galay_core_coro_file_watcher_watch(
    GalayCoreFileWatcher* watcher,
    GalayCoreIOScheduler* scheduler,
    GalayCoreCoroFileWatchResult* out_result,
    int64_t timeout_ms,
    void* user_data,
    const GalayCoreCoroWaitOps* wait_ops);

#ifdef __cplusplus
}
#endif

#endif
