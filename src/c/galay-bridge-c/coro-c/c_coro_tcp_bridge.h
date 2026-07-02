#ifndef GALAY_KERNEL_CORE_C_CORO_TCP_BRIDGE_H
#define GALAY_KERNEL_CORE_C_CORO_TCP_BRIDGE_H

#include <galay/c/galay-common-c/common/galay_c_iovec.h>

#include <stddef.h>
#include <stdint.h>

/**
 * @file c_coro_tcp_bridge.h
 * @brief C++ kernel core 暴露的 C 协程 TCP adapter。
 *
 * @details 这是 C ABI 层使用的内部 C 风格桥接。它将 C++ awaitable/controller
 * 细节限制在 kernel core 内，同时允许 C coroutine runtime 提供 wait 和 completion hook。
 */

/**
 * @brief bridge host 地址缓存长度。
 *
 * @note 该长度覆盖文本形式 IPv6 地址和结尾 '\0'。调用方传入地址时必须保证
 * address 以 '\0' 结尾，或在整个数组长度内可被当作固定长度字符串读取。
 */
#define GALAY_CORE_CORO_HOST_ADDRESS_MAX_LENGTH 46

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GalayCoreTcpSocket GalayCoreTcpSocket;
typedef struct GalayCoreUdpSocket GalayCoreUdpSocket;
typedef struct GalayCoreAsyncFile GalayCoreAsyncFile;
typedef struct GalayCoreAioFile GalayCoreAioFile;
typedef struct GalayCoreFileWatcher GalayCoreFileWatcher;
typedef struct GalayCoreAsyncMutex GalayCoreAsyncMutex;
typedef struct GalayCoreAsyncWaiter GalayCoreAsyncWaiter;
typedef struct GalayCoreIOScheduler GalayCoreIOScheduler;

/**
 * @brief bridge 层 direct coroutine I/O 结果码。
 *
 * @details 与 public C ABI 的 C_IOResultCode 一一对应，但保持 bridge 头文件独立。
 * 调用方必须先检查 code，再读取 bytes/value/ptr。
 */
typedef enum GalayCoreCoroIOResultCode {
    GalayCoreCoroIOResultOk,          ///< 操作成功。
    GalayCoreCoroIOResultEof,         ///< 对端关闭或 EOF。
    GalayCoreCoroIOResultTimeout,     ///< 等待超时，或 timeout_ms 为 0。
    GalayCoreCoroIOResultCancelled,   ///< 挂起操作被 close/cancel 取消。
    GalayCoreCoroIOResultInvalid,     ///< 参数、scheduler、wait hook 或状态无效。
    GalayCoreCoroIOResultError,       ///< 底层 I/O 或运行时错误；sys_errno 尽量保留 errno。
} GalayCoreCoroIOResultCode;

/**
 * @brief bridge 层 direct coroutine I/O 结果结构。
 *
 * @note sys_errno 为 0 表示没有可公开的底层 errno。bytes/value/ptr 的含义由具体
 * bridge API 定义。
 */
typedef struct GalayCoreCoroIOResult {
    GalayCoreCoroIOResultCode code;  ///< 完成状态。
    int sys_errno;                   ///< errno/归一化系统错误，非错误结果通常为 0。
    size_t bytes;                    ///< 成功读写字节数或批量结果数。
    int64_t value;                   ///< 附加整数值，例如 accepted fd。
    void* ptr;                       ///< 附加指针，所有权由具体 API 说明。
} GalayCoreCoroIOResult;

/**
 * @brief bridge 层 IP 地址族。
 *
 * @note 枚举值需与 public C ABI 和 C++ kernel IPType 保持一致。
 */
typedef enum GalayCoreCoroIPType {
    GalayCoreCoroIPTypeIPV4 = 0,   ///< IPv4 地址族。
    GalayCoreCoroIPTypeIPV6 = 1,   ///< IPv6 地址族。
} GalayCoreCoroIPType;

/**
 * @brief bridge 层 Host 值类型。
 *
 * @details address 是内联缓存，不由 bridge 保存。作为输出参数时，函数会写入
 * 以 '\0' 结尾的地址字符串和主机字节序端口。
 */
typedef struct GalayCoreCoroHost {
    GalayCoreCoroIPType type;                              ///< IP 地址族。
    char address[GALAY_CORE_CORO_HOST_ADDRESS_MAX_LENGTH]; ///< 以 '\0' 结尾的地址字符串。
    uint16_t port;                                         ///< 主机字节序端口号。
} GalayCoreCoroHost;

/**
 * @brief 等待 runtime token 完成的回调。
 *
 * @param ctx 调用方在 GalayCoreCoroWaitOps::ctx 中提供的上下文。
 * @param timeout_ms 负数无限等待，正数为毫秒超时；0 通常由 bridge 入口提前返回
 * Timeout，不调用 wait。
 * @return runtime 等待结果。返回 Timeout 时 bridge 会尝试撤销挂起 I/O 并释放 token。
 *
 * @note wait 必须让出/挂起当前 C coroutine，不应阻塞 IO scheduler 线程。
 */
typedef GalayCoreCoroIOResult (*GalayCoreCoroWaitFn)(void* ctx, int64_t timeout_ms);

/**
 * @brief 将 I/O 完成结果写回 runtime token 的回调。
 *
 * @param user_data bridge 入口收到的 user_data。
 * @param result 待写回的完成结果。
 * @return Ok 表示 runtime 接收完成结果；Invalid 表示 token 已不可写，bridge 会把它
 * 视作可清理状态；其它错误会合并进最终结果。
 *
 * @note 该回调可能由 IO scheduler 完成路径触发。实现必须自行保证 user_data 的
 * 线程/协程安全。
 */
typedef GalayCoreCoroIOResult (*GalayCoreCoroCompleteUserDataFn)(
    void* user_data,
    GalayCoreCoroIOResult result);

/**
 * @brief 释放 runtime token 的回调。
 *
 * @param user_data bridge 入口收到的 user_data。
 * @return Ok 表示释放成功；其它结果会在可能时合并到最终 C_IOResult。
 *
 * @note complete_user_data 成功或等待超时/取消清理后，bridge 会调用该函数交还 token
 * 生命周期。
 */
typedef GalayCoreCoroIOResult (*GalayCoreCoroReleaseUserDataFn)(void* user_data);

/**
 * @brief C coroutine runtime 提供给 bridge 的等待和 token 生命周期回调表。
 *
 * @details bridge 在提交 awaitable 后调用 wait(ctx, timeout_ms) 挂起当前 C coroutine。
 * I/O 完成、取消或清理路径通过 complete_user_data/release_user_data 回写并释放
 * user_data。
 *
 * @note wait、complete_user_data、release_user_data 必须全部非 NULL；ctx 可为 NULL，
 * 由调用方自定义。允许调用线程：wait 只能由当前 C coroutine 所属 owner IO
 * scheduler 线程调用；complete_user_data/release_user_data 可由 IO scheduler 完成路径
 * 或清理路径调用。可重入：同一 user_data token 不可并发重入，跨 token 调用必须由
 * runtime 自行保证线程安全。
 */
typedef struct GalayCoreCoroWaitOps {
    GalayCoreCoroWaitFn wait;                              ///< 挂起/等待当前 C coroutine。
    GalayCoreCoroCompleteUserDataFn complete_user_data;    ///< 写回完成结果。
    GalayCoreCoroReleaseUserDataFn release_user_data;      ///< 释放 runtime token。
    void* ctx;                                             ///< runtime 自定义上下文。
} GalayCoreCoroWaitOps;

/**
 * @brief 提交 TCP accept awaitable 并等待完成。
 *
 * @param listener_socket 内部 TcpSocket 指针，必须非 NULL 且处于监听状态。
 * @param scheduler 内部 IOScheduler 指针，必须属于当前 C coroutine。
 * @param out_socket 输出 accepted TcpSocket 指针地址；*out_socket 必须为 NULL。
 * @param out_peer 可选输出 peer 地址；不需要时传 NULL。
 * @param timeout_ms 负数无限等待，0 立即返回 Timeout，正数为毫秒超时。
 * @param user_data runtime token，必须非 NULL，由 wait_ops 管理完成和释放。
 * @param wait_ops runtime 等待/完成回调表，所有回调必须非 NULL。
 * @return 成功返回 Ok，value 为 accepted fd，ptr 指向 out_socket；错误通过
 * GalayCoreCoroIOResultCode 显式返回。
 *
 * @note out_socket/out_peer/user_data 必须在操作完成或被清理前保持有效。close 可取消
 * 同一 controller 上挂起的 direct C coroutine accept。
 */
GalayCoreCoroIOResult galay_core_coro_tcp_accept(GalayCoreTcpSocket* listener_socket,
                                                 GalayCoreIOScheduler* scheduler,
                                                 GalayCoreTcpSocket** out_socket,
                                                 GalayCoreCoroHost* out_peer,
                                                 int64_t timeout_ms,
                                                 void* user_data,
                                                 const GalayCoreCoroWaitOps* wait_ops);

/**
 * @brief 提交 TCP connect awaitable 并等待完成。
 *
 * @param socket 内部 TcpSocket 指针，必须非 NULL。
 * @param scheduler 内部 IOScheduler 指针，必须属于当前 C coroutine。
 * @param host 远端地址，必须非 NULL 且为有效 IPv4/IPv6 host。
 * @param timeout_ms 负数无限等待，0 立即返回 Timeout，正数为毫秒超时。
 * @param user_data runtime token，必须非 NULL。
 * @param wait_ops runtime 等待/完成回调表，所有回调必须非 NULL。
 * @return 成功返回 Ok；参数/状态无效返回 Invalid；超时返回 Timeout；底层错误返回
 * Error/sys_errno。
 *
 * @note socket 会在提交前被设置为非阻塞模式。host 和 user_data 必须在函数返回前
 * 保持有效。
 */
GalayCoreCoroIOResult galay_core_coro_tcp_connect(GalayCoreTcpSocket* socket,
                                                  GalayCoreIOScheduler* scheduler,
                                                  const GalayCoreCoroHost* host,
                                                  int64_t timeout_ms,
                                                  void* user_data,
                                                  const GalayCoreCoroWaitOps* wait_ops);

/**
 * @brief 提交 TCP recv awaitable 并等待完成。
 *
 * @param socket 内部 TcpSocket 指针，必须非 NULL。
 * @param scheduler 内部 IOScheduler 指针，必须属于当前 C coroutine。
 * @param buffer 输出缓冲区，必须非 NULL。
 * @param length 最多接收字节数，必须大于 0。
 * @param timeout_ms 负数无限等待，0 立即返回 Timeout，正数为毫秒超时。
 * @param user_data runtime token，必须非 NULL。
 * @param wait_ops runtime 等待/完成回调表，所有回调必须非 NULL。
 * @return 成功返回 Ok 且 bytes 为读取字节数；对端关闭返回 Eof；其它错误按 code
 * 和 sys_errno 返回。
 *
 * @note buffer 必须在函数返回前保持有效且不被并发写入。
 */
GalayCoreCoroIOResult galay_core_coro_tcp_recv(GalayCoreTcpSocket* socket,
                                               GalayCoreIOScheduler* scheduler,
                                               char* buffer,
                                               size_t length,
                                               int64_t timeout_ms,
                                               void* user_data,
                                               const GalayCoreCoroWaitOps* wait_ops);

/**
 * @brief 提交 TCP send awaitable 并等待完成。
 *
 * @param socket 内部 TcpSocket 指针，必须非 NULL。
 * @param scheduler 内部 IOScheduler 指针，必须属于当前 C coroutine。
 * @param buffer 待发送数据，必须非 NULL。
 * @param length 待发送字节数，必须大于 0。
 * @param timeout_ms 负数无限等待，0 立即返回 Timeout，正数为毫秒超时。
 * @param user_data runtime token，必须非 NULL。
 * @param wait_ops runtime 等待/完成回调表，所有回调必须非 NULL。
 * @return 成功返回 Ok 且 bytes 为发送字节数；其它错误按 code 和 sys_errno 返回。
 *
 * @note buffer 必须在函数返回前保持有效。调用方需要处理短写。
 */
GalayCoreCoroIOResult galay_core_coro_tcp_send(GalayCoreTcpSocket* socket,
                                               GalayCoreIOScheduler* scheduler,
                                               const char* buffer,
                                               size_t length,
                                               int64_t timeout_ms,
                                               void* user_data,
                                               const GalayCoreCoroWaitOps* wait_ops);

/**
 * @brief 提交 TCP readv awaitable 并等待完成。
 *
 * @param socket 内部 TcpSocket 指针，必须非 NULL。
 * @param scheduler 内部 IOScheduler 指针，必须属于当前 C coroutine。
 * @param iovecs 输出 galay_iovec_t 数组，必须非 NULL。
 * @param count iovec 数量，必须大于 0。
 * @param timeout_ms 负数无限等待，0 立即返回 Timeout，正数为毫秒超时。
 * @param user_data runtime token，必须非 NULL。
 * @param wait_ops runtime 等待/完成回调表，所有回调必须非 NULL。
 * @return 成功返回 Ok 且 bytes 为读取字节数；其它错误按 code 和 sys_errno 返回。
 *
 * @note iovecs 和 base 指向的缓冲区必须在函数返回前保持有效。
 */
GalayCoreCoroIOResult galay_core_coro_tcp_readv(GalayCoreTcpSocket* socket,
                                                GalayCoreIOScheduler* scheduler,
                                                const galay_iovec_t* iovecs,
                                                size_t count,
                                                int64_t timeout_ms,
                                                void* user_data,
                                                const GalayCoreCoroWaitOps* wait_ops);

/**
 * @brief 提交 TCP writev awaitable 并等待完成。
 *
 * @param socket 内部 TcpSocket 指针，必须非 NULL。
 * @param scheduler 内部 IOScheduler 指针，必须属于当前 C coroutine。
 * @param iovecs 输入 galay_iovec_t 数组，必须非 NULL。
 * @param count iovec 数量，必须大于 0。
 * @param timeout_ms 负数无限等待，0 立即返回 Timeout，正数为毫秒超时。
 * @param user_data runtime token，必须非 NULL。
 * @param wait_ops runtime 等待/完成回调表，所有回调必须非 NULL。
 * @return 成功返回 Ok 且 bytes 为写入字节数；其它错误按 code 和 sys_errno 返回。
 *
 * @note iovecs 和 base 指向的数据必须在函数返回前保持有效。调用方需要处理短写。
 */
GalayCoreCoroIOResult galay_core_coro_tcp_writev(GalayCoreTcpSocket* socket,
                                                 GalayCoreIOScheduler* scheduler,
                                                 const galay_iovec_t* iovecs,
                                                 size_t count,
                                                 int64_t timeout_ms,
                                                 void* user_data,
                                                 const GalayCoreCoroWaitOps* wait_ops);

/**
 * @brief 提交 TCP sendfile awaitable 并等待完成。
 *
 * @param socket 内部 TcpSocket 指针，必须非 NULL。
 * @param scheduler 内部 IOScheduler 指针，必须属于当前 C coroutine。
 * @param file_fd 待读取文件描述符，必须非负，调用方仍拥有该 fd。
 * @param offset 文件偏移，必须非负。
 * @param count 最多发送字节数，必须大于 0。
 * @param timeout_ms 负数无限等待，0 立即返回 Timeout，正数为毫秒超时。
 * @param user_data runtime token，必须非 NULL。
 * @param wait_ops runtime 等待/完成回调表，所有回调必须非 NULL。
 * @return 成功返回 Ok 且 bytes 为发送字节数；其它错误按 code 和 sys_errno 返回。
 *
 * @note socket 和 file_fd 必须在函数返回前保持有效。
 */
GalayCoreCoroIOResult galay_core_coro_tcp_sendfile(GalayCoreTcpSocket* socket,
                                                   GalayCoreIOScheduler* scheduler,
                                                   int file_fd,
                                                   int64_t offset,
                                                   size_t count,
                                                   int64_t timeout_ms,
                                                   void* user_data,
                                                   const GalayCoreCoroWaitOps* wait_ops);

/**
 * @brief 在 IOScheduler 上关闭 TCP socket。
 *
 * @param socket 内部 TcpSocket 指针，必须非 NULL。
 * @param scheduler 内部 IOScheduler 指针，必须与 socket controller 所属 scheduler 匹配。
 * @param timeout_ms 为 ABI 对称保留；当前实现不等待 timeout，只执行 close 注册。
 * @return 成功返回 Ok；参数、scheduler 或挂起操作状态不允许关闭时返回 Invalid；
 * close 注册失败返回 Error/sys_errno。
 *
 * @note close 会取消同一 controller 上挂起的 direct C coroutine 操作并回写 Cancelled。
 * 不允许在存在非 direct C coroutine awaitable 时关闭。
 */
GalayCoreCoroIOResult galay_core_coro_tcp_close(GalayCoreTcpSocket* socket,
                                                GalayCoreIOScheduler* scheduler,
                                                int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
