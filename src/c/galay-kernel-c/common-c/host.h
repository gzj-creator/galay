#ifndef GALAY_KERNEL_HOST_C_H
#define GALAY_KERNEL_HOST_C_H

#include <stdint.h>

/**
 * @file host.h
 * @brief Galay kernel Host 的 C ABI 值类型。
 *
 * @details 对应 C++ galay::kernel::Host 的 C 侧表示，用于在 C ABI 边界传递
 * IPv4/IPv6 地址和端口。C 侧不暴露 sockaddr_storage，具体转换由实现文件完成。
 */

#define C_HOST_ADDRESS_MAX_LENGTH 46

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief IP 协议版本。
 *
 * @note 枚举值与 C++ galay::kernel::IPType 保持一致。
 */
typedef enum C_IPType {
    IPV4 = 0,   ///< IPv4 地址族。
    IPV6 = 1    ///< IPv6 地址族。
} C_IPType;

/**
 * @brief C ABI Host 值类型。
 *
 * @details address 使用固定长度内联缓存，既可作为输入配置，也可作为输出端点。
 * IPv4 地址最大长度为 16 字节，IPv6 地址最大长度为 46 字节，包含结尾 '\0'。
 */
typedef struct C_Host {
    C_IPType type;                          ///< IP 协议版本。
    char address[C_HOST_ADDRESS_MAX_LENGTH]; ///< 以 '\0' 结尾的 IPv4/IPv6 地址字符串。
    uint16_t port;                          ///< 主机字节序端口号。
} C_Host;

#ifdef __cplusplus
}
#endif

#endif
