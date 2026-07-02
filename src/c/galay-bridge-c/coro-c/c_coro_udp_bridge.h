#ifndef GALAY_KERNEL_CORE_C_CORO_UDP_BRIDGE_H
#define GALAY_KERNEL_CORE_C_CORO_UDP_BRIDGE_H

#include "c_coro_tcp_bridge.h"

/**
 * @file c_coro_udp_bridge.h
 * @brief C++ kernel core 暴露的 C 协程 UDP adapter。
 *
 * @details 该内部 C 风格桥接复用 TCP bridge 的 C_IOResult/Host/wait hook
 * ABI，只把 C++ UDP awaitable/controller 细节限制在 kernel core 内。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 提交 UDP recvfrom awaitable 并等待完成。
 *
 * @param socket 内部 UdpSocket 指针，必须非 NULL。
 * @param scheduler 内部 IOScheduler 指针，必须属于当前 C coroutine。
 * @param buffer 输出缓冲区；length 为 0 时可为 NULL。
 * @param length 最多接收字节数。
 * @param from 可选输出发送端地址；不需要时传 NULL。
 * @param timeout_ms 负数无限等待，0 立即返回 Timeout，正数为毫秒超时。
 * @param user_data runtime token，必须非 NULL，由 wait_ops 管理完成和释放。
 * @param wait_ops runtime 等待/完成回调表，所有回调必须非 NULL。
 * @return 成功返回 Ok 且 bytes 为接收字节数；超时返回 Timeout；参数、scheduler
 * 或 wait hook 无效返回 Invalid；底层错误返回 Error/sys_errno。
 *
 * @note buffer/from/user_data 必须在函数返回或清理完成前保持有效。close 可取消同一
 * controller 上挂起的 direct C coroutine recvfrom。
 */
GalayCoreCoroIOResult galay_core_coro_udp_recvfrom(GalayCoreUdpSocket* socket,
                                                   GalayCoreIOScheduler* scheduler,
                                                   char* buffer,
                                                   size_t length,
                                                   GalayCoreCoroHost* from,
                                                   int64_t timeout_ms,
                                                   void* user_data,
                                                   const GalayCoreCoroWaitOps* wait_ops);

/**
 * @brief 提交 UDP sendto awaitable 并等待完成。
 *
 * @param socket 内部 UdpSocket 指针，必须非 NULL。
 * @param scheduler 内部 IOScheduler 指针，必须属于当前 C coroutine。
 * @param buffer 待发送数据；length 为 0 时可为 NULL。
 * @param length 待发送字节数。
 * @param to 目标地址，必须非 NULL 且为有效 IPv4/IPv6 host。
 * @param timeout_ms 负数无限等待，0 立即返回 Timeout，正数为毫秒超时。
 * @param user_data runtime token，必须非 NULL，由 wait_ops 管理完成和释放。
 * @param wait_ops runtime 等待/完成回调表，所有回调必须非 NULL。
 * @return 成功返回 Ok 且 bytes 为发送字节数；超时返回 Timeout；参数、scheduler
 * 或 wait hook 无效返回 Invalid；底层错误返回 Error/sys_errno。
 *
 * @note buffer/to/user_data 必须在函数返回前保持有效。
 */
GalayCoreCoroIOResult galay_core_coro_udp_sendto(GalayCoreUdpSocket* socket,
                                                 GalayCoreIOScheduler* scheduler,
                                                 const char* buffer,
                                                 size_t length,
                                                 const GalayCoreCoroHost* to,
                                                 int64_t timeout_ms,
                                                 void* user_data,
                                                 const GalayCoreCoroWaitOps* wait_ops);

/**
 * @brief 在 IOScheduler 上关闭 UDP socket。
 *
 * @param socket 内部 UdpSocket 指针，必须非 NULL。
 * @param scheduler 内部 IOScheduler 指针，必须与 socket controller 所属 scheduler 匹配。
 * @param timeout_ms 为 ABI 对称保留；当前实现不等待 timeout，只执行 close 注册。
 * @return 成功返回 Ok；参数、scheduler 或挂起操作状态不允许关闭时返回 Invalid；
 * close 注册失败返回 Error/sys_errno。
 *
 * @note close 会取消同一 controller 上挂起的 direct C coroutine recvfrom/sendto。
 * 不允许在存在非 direct C coroutine awaitable 时关闭。
 */
GalayCoreCoroIOResult galay_core_coro_udp_close(GalayCoreUdpSocket* socket,
                                                GalayCoreIOScheduler* scheduler,
                                                int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
