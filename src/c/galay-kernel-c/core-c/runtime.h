#ifndef GALAY_C_KERNEL_CORE_RUNTIME_H
#define GALAY_C_KERNEL_CORE_RUNTIME_H

#include "../common-c/macro.h"

#include <stdbool.h>
#include <stddef.h>

/**
 * @file runtime.h
 * @brief Galay kernel runtime 的 C ABI 句柄定义。
 *
 * @details runtime 是驱动异步 IO 和任务调度的核心对象。C ABI 只暴露稳定的
 * void* 载荷，实际对象为 C runtime 的私有实现。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief runtime C ABI 操作结果码。
 *
 * @note 枚举值带 C_Runtime 前缀，避免和其他 C ABI 模块的 enum 成员重名。
 */
typedef enum C_RuntimeResultCode {
    C_RuntimeSuccess,              ///< 操作成功。
    C_RuntimeParameterInvalid,     ///< 参数错误。
    C_RuntimeMemoryAllocFailed,    ///< 内存分配失败。
    C_RuntimeStartFailed,          ///< runtime 启动或 scheduler 线程收尾失败。
} C_RuntimeResultCode;

/**
 * @brief runtime 创建配置。
 *
 * @details 当 scheduler 数为 C_RUNTIME_SCHEDULER_COUNT_AUTO 时，底层 Runtime
 * 会按机器 CPU 数自动推导默认 scheduler 数量。
 */
typedef struct C_RuntimeConfig {
    size_t io_scheduler_count;      ///< IO scheduler 数量。
    size_t parallel_scheduler_count; ///< parallel scheduler 数量。
} C_RuntimeConfig;

/**
 * @brief runtime C 句柄。
 *
 * @note runtime 指向内部 C runtime 对象，调用方不能解引用或直接释放。
 */
typedef struct galay_c_runtime {
    void* runtime;          ///< 内部 Runtime 对象指针。
} galay_c_runtime_t;

/**
 * @brief 将 runtime 结果码转换为可读错误信息。
 *
 * @param code C_RuntimeResultCode 结果码。
 * @return 指向静态错误字符串的指针，调用方不需要释放。
 */
const char* galay_c_runtime_get_error(C_RuntimeResultCode code);

/**
 * @brief 返回默认 runtime 配置。
 *
 * @return 默认配置；IO 与 parallel scheduler 数均为自动推导。
 */
C_RuntimeConfig galay_c_runtime_config_default(void);

/**
 * @brief 创建 runtime。
 *
 * @param config runtime 创建配置。
 * @param c_runtime 输出 runtime 句柄；成功时其 runtime 字段指向内部 runtime。
 * @return 成功返回 C_RuntimeSuccess；参数无效返回 C_RuntimeParameterInvalid；
 * 内存分配失败返回 C_RuntimeMemoryAllocFailed。
 *
 * @note 只创建对象，不启动 scheduler。
 */
C_RuntimeResultCode galay_c_runtime_create(
    const C_RuntimeConfig* config,
    galay_c_runtime_t* c_runtime);

/**
 * @brief 启动 runtime。
 *
 * @param c_runtime 由 galay_c_runtime_create 初始化的 runtime 句柄。
 * @return 成功返回 C_RuntimeSuccess；参数无效返回 C_RuntimeParameterInvalid；
 * 启动失败返回 C_RuntimeStartFailed。
 */
C_RuntimeResultCode galay_c_runtime_start(galay_c_runtime_t* c_runtime);

/**
 * @brief 停止 runtime。
 *
 * @param c_runtime 由 galay_c_runtime_create 初始化的 runtime 句柄。
 * @return 成功返回 C_RuntimeSuccess；参数无效返回 C_RuntimeParameterInvalid；
 * scheduler 线程异常退出或 join 失败返回 C_RuntimeStartFailed。
 */
C_RuntimeResultCode galay_c_runtime_stop(galay_c_runtime_t* c_runtime);

/**
 * @brief 查询 runtime 是否正在运行。
 *
 * @param c_runtime runtime 句柄；为空或未初始化时返回 false。
 * @return 正在运行返回 true，否则返回 false。
 */
bool galay_c_runtime_is_running(const galay_c_runtime_t* c_runtime);

/**
 * @brief 销毁 runtime。
 *
 * @param c_runtime 由 galay_c_runtime_create 初始化的 runtime 句柄。
 * @return 成功返回 C_RuntimeSuccess；参数无效返回 C_RuntimeParameterInvalid；
 * scheduler 销毁失败返回 C_RuntimeStartFailed。
 *
 * @note 会释放 c_runtime->runtime 指向的内部 runtime，并将其置空；已为空的有效
 * 句柄可重复销毁并返回成功。
 */
C_RuntimeResultCode galay_c_runtime_destroy(galay_c_runtime_t* c_runtime);

#ifdef __cplusplus
}
#endif

#endif
