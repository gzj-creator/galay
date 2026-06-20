#ifndef GALAY_HTTP_PLUGIN_DEFN_H
#define GALAY_HTTP_PLUGIN_DEFN_H

#include "../../../galay-kernel/common/host.hpp"
#include "../../../galay-kernel/core/task.h"
#include "../../../galay-kernel/core/runtime.h"

namespace galay::http::plugin {

/**
 * @brief HTTP 服务端 accept 阶段插件基类。
 * @tparam SocketType 接入得到的 socket 类型，例如 TcpSocket 或 SslSocket。
 * @details
 * - server 注册成功后接管插件实例生命周期，插件实例不得同时注册到多个 server。
 * - `start()` 在 runtime 启动后、accept loop 投递前按注册顺序调用。
 * - `stop()` 在 server 停止 runtime 前按注册反序调用，必须保证 noexcept。
 * - `handle()` 在每次 accept 后、协议连接包装前按注册顺序调用；返回 false 会中断后续插件并跳过业务处理。
 * - socket 与 client_host 引用只在当前 accept 的 await 链内有效，插件不得保存悬空引用。
 * - 插件成员会被同一 server 上的并发 accept 共享，存在可变状态时必须自行保证并发安全。
 */
template<typename SocketType>
class AcceptPlugin {
public:
    virtual ~AcceptPlugin() = default;

    /**
     * @brief 启动插件。
     * @param runtime server 已启动的 runtime。
     * @return 返回 true 表示插件启动成功；返回 false 会使 server start 失败。
     */
    virtual bool start(galay::kernel::Runtime& runtime) {
        (void)runtime;
        return true;
    }

    /**
     * @brief 停止插件并释放运行期资源。
     * @details server 会忽略并记录 stop 中抛出的异常；实现仍必须保持 noexcept。
     */
    virtual void stop() noexcept {}

    /**
     * @brief 处理单次 accept 得到的 socket。
     * @param runtime server runtime。
     * @param socket 当前 accept 得到的 socket，仅在本次 await 链内有效。
     * @param client_host 当前 accept 得到的客户端地址，仅在本次 await 链内有效。
     * @return 返回 true 继续执行后续插件和业务处理；返回 false 中断当前连接处理。
     */
    virtual galay::kernel::Task<bool> handle(
        galay::kernel::Runtime& runtime,
        SocketType& socket,
        const galay::kernel::Host& client_host) = 0;
};

}


#endif
