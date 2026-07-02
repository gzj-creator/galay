#ifndef GALAY_KERNEL_CORO_RESULT_C_H
#define GALAY_KERNEL_CORO_RESULT_C_H

#include <stdint.h>
#include <stddef.h>
#include <galay/c/galay-common-c/common/galay_c_error.h>

/**
 * @file coro_result_c.h
 * @brief Galay kernel C coroutine 的通用 I/O 结果结构。
 *
 * @details C coroutine ABI 通过 C_IOResult 显式返回完成状态、errno、字节数和
 * 附加值；不会跨 C ABI 抛出 C++ 异常。调用方必须检查 code 后再读取其它字段。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief C 协程和 I/O 结果码。
 *
 * @details Ok/Eof/Timeout/Cancelled/Invalid/Error 覆盖 direct C coroutine I/O、
 * channel yield 和 bridge completion。具体函数会在自己的注释中说明哪些字段有效。
 */
typedef enum C_IOResultCode {
    C_IOResultOk,          ///< 操作成功；bytes/value/ptr 按具体 API 语义读取。
    C_IOResultEof,         ///< 对端关闭、文件结束或读路径收到断开信号。
    C_IOResultTimeout,     ///< timeout_ms 到期，或 timeout_ms 为 0 时立即超时。
    C_IOResultCancelled,   ///< 等待中的操作被 close/cancel 或运行时取消。
    C_IOResultInvalid,     ///< 参数、句柄、调度器、协程上下文或对象状态无效。
    C_IOResultError        ///< 底层 I/O 或运行时错误；sys_errno 尽量保留系统 errno。
} C_IOResultCode;

/**
 * @brief C 协程结果结构。
 *
 * @details 恢复路径不会通过参数传递业务结果。被恢复的协程从自己的 request/task
 * 状态中读取结果结构，再由 direct C ABI 返回给调用方。ABI 演进只能在尾部追加
 * 字段，禁止在现有字段之间插入或改变字段含义。
 *
 * @note sys_errno 仅在 code 为 C_IOResultError、部分 C_IOResultInvalid 或
 * C_IOResultTimeout 时可能有诊断意义；为 0 表示没有可公开的系统 errno。
 */
typedef struct C_IOResult {
    C_IOResultCode code;   ///< 完成状态，调用方必须首先检查该字段。
    int sys_errno;         ///< 归一化后的 errno/系统错误；无底层错误时为 0。
    size_t bytes;          ///< 成功读写/批量处理的字节数或条目数，按 API 约定解释。
    int64_t value;         ///< 附加整数值，例如文件描述符或计数；无语义时为 0。
    void* ptr;             ///< 附加指针值；所有权按具体 API 注释约定。
} C_IOResult;

/**
 * @brief 返回 C_IOResultCode 的稳定字符串。
 *
 * @param code C 协程/I/O 结果码，允许传入未知枚举值。
 * @return 静态字符串；不会返回 NULL，未知值返回 "unknown"。
 * @note 该函数不分配内存、不会阻塞，适合 FFI 绑定在错误转换路径直接调用。
 */
const char* galay_coro_ioresult_string(C_IOResultCode code);

/**
 * @brief 将 C_IOResultCode 归一化为 galay_status_t。
 *
 * @param code C 协程/I/O 结果码，允许传入未知枚举值。
 * @return 可供只处理通用状态码的上层使用的状态；未知值返回
 * GALAY_INTERNAL_ERROR。
 * @note 该函数不分配内存、不会阻塞，保留 EOF/Timeout/Cancelled 的通用状态区分。
 */
galay_status_t galay_coro_ioresult_to_status(C_IOResultCode code);

#ifdef __cplusplus
}
#endif

#endif
