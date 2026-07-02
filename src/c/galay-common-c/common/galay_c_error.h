#ifndef GALAY_C_COMMON_GALAY_C_ERROR_H
#define GALAY_C_COMMON_GALAY_C_ERROR_H

#include "galay_c_defs.h"

/**
 * @file galay_c_error.h
 * @brief Galay C ABI 共享版本号和通用错误码。
 *
 * @details C API 通过显式状态码或结果结构返回可恢复错误；不会通过 C++ 异常
 * 穿过 ABI 边界。错误字符串函数返回静态只读字符串，调用方不拥有其内存。
 */

/**
 * @brief Galay C ABI 头文件版本号。
 *
 * @note 这些宏表示当前 C ABI 头文件声明版本；运行时库版本请通过
 * galay_c_version_major/minor/patch 查询并与宏值比较。
 */
#define GALAY_C_VERSION_MAJOR 6u
#define GALAY_C_VERSION_MINOR 0u
#define GALAY_C_VERSION_PATCH 0u

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Galay C ABI 通用状态码。
 *
 * @details 模块专用错误码会在各自头文件中定义；公共模块可直接返回该枚举。
 * 所有枚举值都可通过 galay_status_string 或 galay_c_common_get_error 转为静态
 * 错误字符串。
 */
typedef enum galay_status_t {
    GALAY_OK = 0,                  ///< 操作成功。
    GALAY_INVALID_ARGUMENT = 1,    ///< 参数为空、越界、格式非法或状态不满足前置条件。
    GALAY_NOT_FOUND = 2,           ///< 请求的对象、键、资源或路径不存在。
    GALAY_OUT_OF_MEMORY = 3,       ///< 内存分配失败，或调用方提供的输出缓冲区过小。
    GALAY_PROTOCOL_ERROR = 4,      ///< 输入数据不符合对应协议或序列化格式。
    GALAY_UNSUPPORTED = 5,         ///< 当前平台、后端或构建选项不支持该操作。
    GALAY_IO_ERROR = 6,            ///< 底层系统调用、文件、网络或设备 I/O 失败。
    GALAY_INTERNAL_ERROR = 7,      ///< 内部状态不一致或无法归类的实现错误。
    GALAY_EOF = 8,                 ///< 对端关闭、文件结束或读路径收到断开信号。
    GALAY_TIMEOUT = 9,             ///< 等待或 I/O 操作超时。
    GALAY_CANCELLED = 10           ///< 等待中的操作被取消。
} galay_status_t;

/**
 * @brief 获取 Galay C ABI 主版本号。
 *
 * @return 当前链接库的 C ABI major 版本。
 *
 * @note 该函数不分配内存、不会阻塞，线程安全。
 */
uint32_t galay_c_version_major( void );

/**
 * @brief 获取 Galay C ABI 次版本号。
 *
 * @return 当前链接库的 C ABI minor 版本。
 *
 * @note 该函数不分配内存、不会阻塞，线程安全。
 */
uint32_t galay_c_version_minor( void );

/**
 * @brief 获取 Galay C ABI patch 版本号。
 *
 * @return 当前链接库的 C ABI patch 版本。
 *
 * @note 该函数不分配内存、不会阻塞，线程安全。
 */
uint32_t galay_c_version_patch( void );

/**
 * @brief 将通用状态码转换为可读字符串。
 *
 * @param status 待转换的 galay_status_t 状态码。
 * @return 指向静态只读错误字符串的指针；未知状态码返回 "unknown"。
 *
 * @note 返回指针由库拥有，调用方不得释放或写入。该函数不分配内存、不会阻塞，
 * 线程安全。
 */
const char* galay_status_string(galay_status_t status);

/**
 * @brief common 模块错误字符串入口。
 *
 * @param status 待转换的 galay_status_t 状态码。
 * @return 指向静态只读错误字符串的指针；语义与 galay_status_string 相同。
 *
 * @note 该函数用于满足每个 C 模块暴露 `*_get_error` 的约定。
 */
const char* galay_c_common_get_error(galay_status_t status);

#ifdef __cplusplus
}
#endif

#endif
