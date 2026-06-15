//
// 由龚智杰于 2026/6/8 创建。
//

#ifndef GALAY_HTTP_BLACKLIST_HPP
#define GALAY_HTTP_BLACKLIST_HPP

#include "galay-http/common/http_log.h"
#include "galay-http/plugin/common/conn_info_storage.hpp"
#include "galay-http/plugin/common/defn.h"

#include <galay-kernel/concurrency/async_mutex.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <limits>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>

namespace galay::http::plugin {

/**
 * @brief HTTP 接入黑名单插件配置。
 * @details 默认按客户端 IP 聚合连接尝试，忽略 TCP 临时源端口。配置支持两类策略：
 *          固定间隔内超过允许次数后临时封禁，以及轻量衰减计数限速。
 */
struct BlackListConfig {
    enum class ClientKeyMode {
        IpOnly,   ///< 按客户端 IP 统计，忽略临时源端口；生产默认值。
        IpAndPort ///< 按完整远端地址统计，主要用于调试或特殊协议场景。
    };

    /**
     * @brief 固定间隔封禁策略。
     * @details 同一地址在 interval 内最多允许 max_attempts_per_interval 次连接；
     *          下一次连接会触发临时封禁。封禁到期后可选择清零 ConnInfo::ip_conn_times。
     */
    struct IntervalBlockPolicy {
        std::size_t max_attempts_per_interval = 20; ///< 单个统计间隔内允许的最大连接次数。
        std::chrono::milliseconds interval{std::chrono::seconds(60)}; ///< 统计间隔长度。
        std::chrono::milliseconds block_duration{std::chrono::minutes(10)}; ///< 超限后的临时封禁时长。
        bool reset_counter_after_unblock = true; ///< 封禁到期后是否清零计数并开启新窗口。
    };

    /**
     * @brief 衰减计数策略。
     * @details 每次连接增加 ConnInfo::ip_conn_times；每经过 decay_interval 按 decay_step
     *          衰减。计数恢复到阈值以下后，客户端会自然重新获得访问机会。
     */
    struct DecayCounterPolicy {
        std::size_t max_attempts = 20; ///< 当前压力计数允许的最大值。
        std::chrono::milliseconds decay_interval{std::chrono::seconds(1)}; ///< 每次衰减的时间间隔。
        std::size_t decay_step = 1; ///< 每个衰减间隔减少的计数。
        std::size_t max_counter_value = 1000; ///< 计数上限，避免异常流量导致无界增长。
    };

    using Policy = std::variant<IntervalBlockPolicy, DecayCounterPolicy>;


    /**
     * @brief 设置超限后是否主动关闭 socket。
     * @param auto_close true 则在拦截时关闭 socket，false 则仅拦截不关闭。
     * @return *this，支持链式调用。
     */
    BlackListConfig& autoClose(const bool auto_close) {
        close_blocked_socket = auto_close;
        return *this;
    }

    /**
     * @brief 设置封禁策略。
     * @param p IntervalBlockPolicy 或 DecayCounterPolicy。
     * @return *this，支持链式调用。
     */
    BlackListConfig& setPolicy(const Policy &p) {
        policy = p;
        return *this;
    }

    /**
     * @brief 设置客户端地址聚合方式。
     * @param mode IpOnly 忽略端口，IpAndPort 按完整地址统计。
     * @return *this，支持链式调用。
     */
    BlackListConfig& clientKeyMode(ClientKeyMode mode) {
        client_key_mode = mode;
        return *this;
    }

    /**
     * @brief 追加单个豁免 IP。
     * @param ip 不受 blacklist 策略限制的客户端 IP。
     * @return *this，支持链式调用。
     */
    BlackListConfig& excludeIp(const std::string &ip) {
        exclude_ips.insert(ip);
        return *this;
    }

    /**
     * @brief 整体替换豁免 IP 列表。
     * @param ips 不受 blacklist 策略限制的客户端 IP 集合。
     * @return *this，支持链式调用。
     */
    BlackListConfig& excludeIps(std::unordered_set<std::string> ips) {
        exclude_ips = std::move(ips);
        return *this;
    }

    Policy policy = IntervalBlockPolicy{}; ///< 当前 blacklist 策略。
    ClientKeyMode client_key_mode = ClientKeyMode::IpOnly; ///< 客户端地址聚合方式。
    bool close_blocked_socket = true; ///< 返回 false 前是否关闭已接入的 socket。
    std::unordered_set<std::string> exclude_ips; ///< 不受 blacklist 策略限制的客户端 IP 列表。
};

/**
 * @brief 基于 accept plugin 的 HTTP 接入黑名单插件。
 * @tparam SocketType 接入阶段传入的 socket 类型，例如 TcpSocket 或 SslSocket。
 * @details 同一 SocketType 的所有 BlackList 插件实例共享一份 ConnInfoStorage 和
 *          AsyncMutex，确保多 serverLoop、多插件实例对同一客户端地址使用同一份统计。
 *          配置归插件实例私有；插件只在 accept 阶段决定是否继续后续插件/业务处理，
 *          不读取 HTTP 请求内容。
 */
template<typename SocketType>
class BlackList final : public AcceptPlugin<SocketType> {
public:
    /**
     * @brief 使用显式配置创建黑名单插件实例。
     * @param config 黑名单行为配置；配置归插件实例私有，统计状态按 SocketType 全局共享。
     */
    explicit BlackList(BlackListConfig config = BlackListConfig{})
        : m_config(std::move(config)) {}

    /**
     * @brief 使用默认固定间隔策略创建黑名单插件实例。
     * @param connection_count_limit 默认统计间隔内每个客户端 IP 允许的最大连接次数；
     *        `0` 表示拦截所有连接。
     * @details 该构造函数会生成 IntervalBlockPolicy，只覆盖 max_attempts_per_interval，
     *          其余字段使用 BlackListConfig 默认值。
     */
    explicit BlackList(std::size_t connection_count_limit)
        : m_config(makeLimitConfig(connection_count_limit)) {}


    /**
     * @brief 启动插件。
     * @param runtime server 已启动的 runtime。
     * @return 返回 true 表示插件启动成功；返回 false 会使 server start 失败。
     */
    bool start(galay::kernel::Runtime& runtime) override {
        return true;
    }

    /**
     * @brief 停止插件并释放运行期资源。
     * @details server 会忽略并记录 stop 中抛出的异常；实现仍必须保持 noexcept。
     */
    void stop() noexcept override {}

    /**
     * @brief 处理单次 accept 得到的 socket。
     * @param runtime server runtime；当前实现不需要访问 runtime。
     * @param socket 当前 accept 得到的 socket；拦截且配置 auto close 时会主动关闭。
     * @param host 当前 accept 得到的客户端地址。
     * @return 策略允许返回 true；超限、锁失败或关闭前拦截返回 false。
     * @details 若 close_blocked_socket 为 true，插件会在返回 false 前主动关闭当前 socket。
     *          AsyncMutex 正常不会失败；若底层等待体返回异常，插件会记录错误并拦截连接。
     */
    kernel::Task<bool> handle(
        galay::kernel::Runtime& runtime,
        SocketType& socket,
        const galay::kernel::Host& host) override {
        (void)runtime;

        if (m_config.exclude_ips.contains(host.ip())) {
            co_return true;
        }

        auto lock_result = co_await m_mutex.lock();
        if (!lock_result) {
            HTTP_LOG_ERROR("[blacklist] [lock-fail]",
                           "ip={} close={} error={}",
                           host.ip(),
                           m_config.close_blocked_socket,
                           lock_result.error().message());
            if (m_config.close_blocked_socket) {
                auto closed = co_await socket.close();
                if (!closed) {
                    HTTP_LOG_ERROR("[blacklist] [close-fail]",
                                   "ip={} close={} error={}",
                                   host.ip(),
                                   m_config.close_blocked_socket,
                                   closed.error().message());
                }
            }
            m_mutex.unlock();
            co_return false;
        }

        const bool include_port =
            m_config.client_key_mode == BlackListConfig::ClientKeyMode::IpAndPort;
        ConnInfo& conn_info = m_storage.getOrCreateConnInfo(host, include_port);
        const auto now = std::chrono::steady_clock::now();
        const bool allowed = std::visit(
            [&](const auto& policy) {
                return applyPolicy(conn_info, policy, now);
            },
            m_config.policy);
        m_mutex.unlock();

        if (!allowed && m_config.close_blocked_socket) {
            auto closed = co_await socket.close();
            if (!closed) {
                HTTP_LOG_ERROR("[blacklist] [close-fail]",
                               "ip={} close={} error={}",
                               host.ip(),
                               m_config.close_blocked_socket,
                               closed.error().message());
            }
        }
        co_return allowed;
    }

    /**
     * @brief 清空当前 SocketType 对应的全局连接统计。
     * @details 该接口不获取异步锁，只能在没有 BlackList 插件正在运行时调用，
     *          例如测试用例之间、服务器启动前或服务器完全停止后。运行中调用会与
     *          accept hook 并发访问 m_storage，属于调用方错误。
     */
    static void clearConnInfo() {
        m_storage.clearConnInfo();
    }

private:
    using TimePoint = std::chrono::steady_clock::time_point;

    static BlackListConfig makeLimitConfig(std::size_t connection_count_limit) {
        BlackListConfig config;
        BlackListConfig::IntervalBlockPolicy policy;
        policy.max_attempts_per_interval = connection_count_limit;
        config.policy = policy;
        return config;
    }

    /**
     * @brief 判断 time_point 是否已经被初始化为有效业务时间。
     */
    static bool isSet(TimePoint value) noexcept {
        return value != TimePoint{};
    }

    /**
     * @brief 对计数做饱和加一，避免高频拒绝路径上发生 size_t 溢出。
     */
    static std::size_t incrementCapped(std::size_t value, std::size_t cap) noexcept {
        if (value >= cap) {
            return cap;
        }
        return value + 1;
    }

    /**
     * @brief 执行固定间隔封禁策略。
     * @param conn_info 当前客户端地址对应的可变状态。
     * @param policy 固定间隔封禁策略配置。
     * @param now 当前 steady_clock 时间，由调用方统一采集。
     * @return 本次连接允许继续返回 true；应被拦截返回 false。
     */
    static bool applyPolicy(ConnInfo& conn_info,
                            const BlackListConfig::IntervalBlockPolicy& policy,
                            TimePoint now) {
        if (isSet(conn_info.blocked_until)) {
            if (now < conn_info.blocked_until) {
                return false;
            }
            conn_info.blocked_until = {};
            if (policy.reset_counter_after_unblock) {
                conn_info.ip_conn_times = 0;
                conn_info.first_access_at = {};
            }
        }

        const bool has_window = isSet(conn_info.first_access_at);
        const bool window_expired =
            has_window && policy.interval > std::chrono::milliseconds::zero() &&
            now - conn_info.first_access_at >= policy.interval;
        if (!has_window || window_expired) {
            conn_info.ip_conn_times = 0;
            conn_info.first_access_at = now;
        }

        if (policy.max_attempts_per_interval == 0) {
            conn_info.blocked_until = now + policy.block_duration;
            return false;
        }

        if (conn_info.ip_conn_times < policy.max_attempts_per_interval) {
            ++conn_info.ip_conn_times;
            return true;
        }

        conn_info.ip_conn_times = incrementCapped(
            conn_info.ip_conn_times, std::numeric_limits<std::size_t>::max());
        conn_info.blocked_until = now + policy.block_duration;
        return false;
    }

    /**
     * @brief 计算衰减策略使用的实际计数上限。
     * @details 即使 max_counter_value 配置过小，也至少保留 max_attempts + 1，
     *          这样计数仍然能够进入拒绝状态。
     */
    static std::size_t counterCap(const BlackListConfig::DecayCounterPolicy& policy) noexcept {
        if (policy.max_attempts == std::numeric_limits<std::size_t>::max()) {
            return std::numeric_limits<std::size_t>::max();
        }
        return std::max(policy.max_counter_value, policy.max_attempts + 1);
    }

    /**
     * @brief 按 elapsed / decay_interval 对 ConnInfo::ip_conn_times 做批量衰减。
     * @param conn_info 当前客户端地址对应的可变状态。
     * @param policy 衰减计数策略配置。
     * @param now 当前 steady_clock 时间，由调用方统一采集。
     * @details 只在访问到该地址时懒衰减，不启动后台定时器。
     */
    static void decayCounter(ConnInfo& conn_info,
                             const BlackListConfig::DecayCounterPolicy& policy,
                             TimePoint now) {
        if (!isSet(conn_info.last_decay_at)) {
            conn_info.last_decay_at = now;
            return;
        }
        if (policy.decay_interval <= std::chrono::milliseconds::zero() ||
            policy.decay_step == 0) {
            return;
        }

        const auto intervals = (now - conn_info.last_decay_at) / policy.decay_interval;
        if (intervals <= 0) {
            return;
        }

        const auto interval_count = static_cast<std::size_t>(intervals);
        const std::size_t max_size = std::numeric_limits<std::size_t>::max();
        const std::size_t total_decay =
            interval_count > max_size / policy.decay_step
                ? max_size
                : interval_count * policy.decay_step;

        if (total_decay >= conn_info.ip_conn_times) {
            conn_info.ip_conn_times = 0;
        } else {
            conn_info.ip_conn_times -= total_decay;
        }
        conn_info.last_decay_at += policy.decay_interval * intervals;
    }

    /**
     * @brief 执行衰减计数策略。
     * @param conn_info 当前客户端地址对应的可变状态。
     * @param policy 衰减计数策略配置。
     * @param now 当前 steady_clock 时间，由调用方统一采集。
     * @return 衰减后加上本次连接仍不超过阈值返回 true；超过阈值返回 false。
     */
    static bool applyPolicy(ConnInfo& conn_info,
                            const BlackListConfig::DecayCounterPolicy& policy,
                            TimePoint now) {
        decayCounter(conn_info, policy, now);
        conn_info.ip_conn_times = incrementCapped(conn_info.ip_conn_times, counterCap(policy));
        return conn_info.ip_conn_times <= policy.max_attempts;
    }

    BlackListConfig m_config;
    static ConnInfoStorage m_storage;
    static galay::kernel::AsyncMutex m_mutex;
};

template<typename SocketType>
ConnInfoStorage BlackList<SocketType>::m_storage = {};

template<typename SocketType>
galay::kernel::AsyncMutex BlackList<SocketType>::m_mutex;

} // namespace galay::http::plugin

#endif // GALAY_HTTP_BLACKLIST_HPP
