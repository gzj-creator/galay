#ifndef GALAY_KERNEL_FILE_WATCHER_C_H
#define GALAY_KERNEL_FILE_WATCHER_C_H

#include "../core-c/runtime_c.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file file_watcher_c.h
 * @brief Galay kernel FileWatcher 的 C ABI 封装。
 *
 * @details 该头文件只暴露 C 可见的轻量句柄、事件位掩码和结果码，
 * 实际文件监控生命周期由实现文件中的 C++ galay::async::FileWatcher 承载。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief FileWatcher C ABI 操作结果码。
 */
typedef enum C_FileWatcherResultCode {
    C_FileWatcherSuccess,               ///< 操作成功。
    C_FileWatcherParameterInvalid,      ///< 参数错误。
    C_FileWatcherMemoryAllocFailed,     ///< 内存分配失败。
    C_FileWatcherIOFailed,              ///< 底层 IO 操作失败。
    C_FileWatcherOperationInvalid,      ///< 当前监控器状态不允许执行该操作。
    C_FileWatcherRuntimeNotRunning,     ///< runtime 未启动。
    C_FileWatcherRuntimeSpawnFailed,    ///< runtime 提交任务失败。
    C_FileWatcherOperationUnsupported,  ///< 当前构建后端不支持文件监控。
    C_FileWatcherTimeout,               ///< watch 操作超时。
} C_FileWatcherResultCode;

/**
 * @brief 文件监控事件位掩码。
 *
 * @note 枚举值与 C++ galay::kernel::FileWatchEvent 保持一致，可按位 OR 组合。
 */
typedef enum C_FileWatchEvent {
    C_FileWatchEventNone = 0,
    C_FileWatchEventAccess = 0x00000001,       ///< 文件被访问。
    C_FileWatchEventModify = 0x00000002,       ///< 文件被修改。
    C_FileWatchEventAttrib = 0x00000004,       ///< 文件属性变化。
    C_FileWatchEventCloseWrite = 0x00000008,   ///< 可写文件关闭。
    C_FileWatchEventCloseNoWrite = 0x00000010, ///< 不可写文件关闭。
    C_FileWatchEventOpen = 0x00000020,         ///< 文件被打开。
    C_FileWatchEventMovedFrom = 0x00000040,    ///< 文件被移出。
    C_FileWatchEventMovedTo = 0x00000080,      ///< 文件被移入。
    C_FileWatchEventCreate = 0x00000100,       ///< 文件被创建。
    C_FileWatchEventDelete = 0x00000200,       ///< 文件被删除。
    C_FileWatchEventDeleteSelf = 0x00000400,   ///< 监控目标被删除。
    C_FileWatchEventMoveSelf = 0x00000800,     ///< 监控目标被移动。
    C_FileWatchEventAll = 0x00000FFF,          ///< 所有文件监控事件。
} C_FileWatchEvent;

/**
 * @brief FileWatcher C 句柄。
 *
 * @note watcher 指向内部 C++ FileWatcher 对象，调用方不能解引用或直接释放。
 */
typedef struct galay_kernel_file_watcher {
    void* watcher;          ///< 内部 FileWatcher 对象指针。
} galay_kernel_file_watcher_t;

/**
 * @brief FileWatcher watch 回调结果。
 *
 * @note name 始终以 '\0' 结尾；在按文件监控或后端不提供文件名时可能为空。
 */
typedef struct galay_kernel_file_watcher_watch_result {
    C_FileWatcherResultCode code;   ///< watch 完成结果码。
    C_FileWatchEvent events;        ///< 触发的文件事件位掩码。
    char name[256];                 ///< 事件关联文件名。
    bool is_dir;                    ///< 事件目标是否为目录。
} galay_kernel_file_watcher_watch_result_t;

/**
 * @brief FileWatcher watch 完成回调。
 *
 * @param result watch 结果；只在回调期间有效。
 * @param ctx 调用 galay_kernel_file_watcher_watch 时传入的用户上下文。
 */
typedef void (*galay_kernel_file_watcher_callback_t)(
    galay_kernel_file_watcher_watch_result_t* result,
    void* ctx);

/**
 * @brief 将 FileWatcher 结果码转换为可读错误信息。
 *
 * @param code C_FileWatcherResultCode 结果码。
 * @return 指向静态错误字符串的指针，调用方不需要释放。
 */
const char* galay_kernel_file_watcher_get_error(C_FileWatcherResultCode code);

/**
 * @brief 创建 FileWatcher。
 *
 * @param c_watcher 输出 watcher 句柄；成功时其 watcher 字段指向内部 FileWatcher。
 * @return 成功返回 C_FileWatcherSuccess；参数无效返回 C_FileWatcherParameterInvalid；
 * 内存分配失败返回 C_FileWatcherMemoryAllocFailed；后端不支持返回 C_FileWatcherOperationUnsupported。
 *
 * @note 该函数只创建监控器对象，不启动协程，也不会阻塞等待文件事件。
 */
C_FileWatcherResultCode galay_kernel_file_watcher_create(
    galay_kernel_file_watcher_t* c_watcher);

/**
 * @brief 销毁 FileWatcher 内部资源。
 *
 * @param c_watcher 由 galay_kernel_file_watcher_create 初始化的 watcher 句柄。
 * @return 成功返回 C_FileWatcherSuccess；参数无效返回 C_FileWatcherParameterInvalid。
 *
 * @note 该函数会释放 c_watcher->watcher 指向的内部 FileWatcher，并将其置空。
 */
C_FileWatcherResultCode galay_kernel_file_watcher_destroy(
    galay_kernel_file_watcher_t* c_watcher);

/**
 * @brief 注册路径以进行文件系统变更监控。
 *
 * @param c_watcher FileWatcher 句柄。
 * @param path 要监控的文件或目录路径；不能为空。
 * @param events 要监控的事件位掩码，必须是 C_FileWatchEvent* 的有效组合。
 * @param watch_descriptor 输出监控描述符。
 * @return 成功返回 C_FileWatcherSuccess；参数无效返回 C_FileWatcherParameterInvalid；
 * IO 失败返回 C_FileWatcherIOFailed；后端不支持返回 C_FileWatcherOperationUnsupported。
 */
C_FileWatcherResultCode galay_kernel_file_watcher_add_watch(
    galay_kernel_file_watcher_t* c_watcher,
    const char* path,
    C_FileWatchEvent events,
    int* watch_descriptor);

/**
 * @brief 移除先前注册的监控。
 *
 * @param c_watcher FileWatcher 句柄。
 * @param watch_descriptor add_watch 返回的监控描述符。
 * @return 成功返回 C_FileWatcherSuccess；参数无效返回 C_FileWatcherParameterInvalid；
 * IO 失败返回 C_FileWatcherIOFailed；后端不支持返回 C_FileWatcherOperationUnsupported。
 */
C_FileWatcherResultCode galay_kernel_file_watcher_remove_watch(
    galay_kernel_file_watcher_t* c_watcher,
    int watch_descriptor);

/**
 * @brief 查询监控描述符关联的路径。
 *
 * @param c_watcher FileWatcher 句柄。
 * @param watch_descriptor add_watch 返回的监控描述符。
 * @param buffer 调用方提供的输出缓冲区。
 * @param buffer_size 输出缓冲区字节数，必须能容纳完整路径和结尾 '\0'。
 * @return 成功返回 C_FileWatcherSuccess；参数无效、描述符不存在或空间不足返回 C_FileWatcherParameterInvalid。
 */
C_FileWatcherResultCode galay_kernel_file_watcher_get_path(
    const galay_kernel_file_watcher_t* c_watcher,
    int watch_descriptor,
    char* buffer,
    size_t buffer_size);

/**
 * @brief 在 runtime 上异步等待一个文件系统事件。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_watcher FileWatcher 句柄；必须存活到 callback 完成。
 * @param callback watch 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_FileWatcherSuccess；参数无效返回 C_FileWatcherParameterInvalid；
 * runtime 未运行返回 C_FileWatcherRuntimeNotRunning；提交失败返回 C_FileWatcherRuntimeSpawnFailed；
 * 后端不支持返回 C_FileWatcherOperationUnsupported。
 *
 * @note 该函数不会阻塞等待文件事件；一次调用只接收一个事件，callback 在 runtime 调度线程上执行。
 *       调用方不应在前一次 watch 完成前对同一 watcher 再次提交 watch。
 */
C_FileWatcherResultCode galay_kernel_file_watcher_watch(
    galay_kernel_runtime_t* runtime,
    galay_kernel_file_watcher_t* c_watcher,
    galay_kernel_file_watcher_callback_t callback,
    void* ctx);

/**
 * @brief 在 runtime 上异步等待一个文件系统事件，带毫秒级超时。
 *
 * @param runtime 已启动的 runtime；必须存活到 callback 完成。
 * @param c_watcher FileWatcher 句柄；必须存活到 callback 完成。
 * @param timeout_ms 超时时间，单位毫秒；0 表示立即超时检查。
 * @param callback watch 完成后调用的回调；不能为空。
 * @param ctx 原样传给 callback 的用户上下文。
 * @return 成功提交返回 C_FileWatcherSuccess；参数无效返回 C_FileWatcherParameterInvalid；
 * runtime 未运行返回 C_FileWatcherRuntimeNotRunning；提交失败返回 C_FileWatcherRuntimeSpawnFailed；
 * 后端不支持返回 C_FileWatcherOperationUnsupported。
 *
 * @note 该函数不会阻塞等待文件事件；超时通过 callback 上报 C_FileWatcherTimeout。
 *       调用方不应在前一次 watch 完成前对同一 watcher 再次提交 watch。
 */
C_FileWatcherResultCode galay_kernel_file_watcher_watch_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_file_watcher_t* c_watcher,
    uint64_t timeout_ms,
    galay_kernel_file_watcher_callback_t callback,
    void* ctx);

#ifdef __cplusplus
}
#endif

#endif
