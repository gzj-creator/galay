#include "redis_session.h"
#include "../base/redis_log.h"
#include "../../galay-utils/system/system.hpp"
#include <regex>
#include <format>
#include <utility>

namespace galay::redis
{
    RedisSession::RedisSession(RedisSessionConfig config)
        : m_config(std::move(config))
        , m_connection(std::make_unique<protocol::Connection>())
    {
    }

    RedisSession::RedisSession(RedisSession&& other) = default;

    RedisSession& RedisSession::operator=(RedisSession&& other) = default;

    std::expected<void, RedisError> RedisSession::connect()
    {
        return connect(m_config.host,
                       m_config.port,
                       m_config.username,
                       m_config.password,
                       m_config.db_index,
                       m_config.version);
    }

    // redis://[username:password@]host[:port][/db_index]
    std::expected<void, RedisError> RedisSession::connect(const std::string &url)
    {
        std::regex pattern(R"(^redis://(?:([^:@]*)(?::([^@]*))?@)?([a-zA-Z0-9\-\.]+)(?::(\d+))?(?:/(\d+))?$)");
        std::smatch matches;
        std::string username, password, host;
        int32_t port = 6379, db_index = 0;

        if (std::regex_match(url, matches, pattern)) {
            if (matches.size() > 1 && !matches[1].str().empty()) {
                username = matches[1];
            }
            if (matches.size() > 2 && !matches[2].str().empty()) {
                password = matches[2];
            }
            if (matches.size() > 3 && !matches[3].str().empty()) {
                host = matches[3];
            } else {
                REDIS_LOG_ERROR("[client]", "[Redis host is invalid]");
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_HOST_INVALID_ERROR));
            }
            if (matches.size() > 4 && !matches[4].str().empty()) {
                try {
                    port = std::stoi(matches[4]);
                } catch(const std::exception& e) {
                    REDIS_LOG_ERROR("[client]", "[Redis port is invalid]");
                    return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_PORT_INVALID_ERROR));
                }
            }
            if (matches.size() > 5 && !matches[5].str().empty()) {
                try {
                    db_index = std::stoi(matches[5]);
                } catch(const std::exception& e) {
                    REDIS_LOG_ERROR("[client]", "[Redis url is invalid]");
                    return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_DB_INDEX_INVALID_ERROR));
                }
            }
        } else {
            REDIS_LOG_ERROR("[client]", "[Redis url is invalid]");
            return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_URL_INVALID_ERROR));
        }

        std::string ip;
        switch (galay::utils::System::check_address_type(host))
        {
        case galay::utils::System::AddressType::IPv4 :
            ip = host;
            break;
        case galay::utils::System::AddressType::IPv6 :
            return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_ADDRESS_TYPE_INVALID_ERROR, "IPv6 is not supported"));
        case galay::utils::System::AddressType::Domain:
        {
            ip = galay::utils::System::resolve_host_ipv4(host);
            if (ip.empty()) {
                REDIS_LOG_ERROR("[client]", "[Get domain's IPV4 failed]");
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_ADDRESS_TYPE_INVALID_ERROR));
            }
            break;
        }
        default:
            REDIS_LOG_ERROR("[client]", "[Unsupported address type]");
            return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_ADDRESS_TYPE_INVALID_ERROR));
        }

        return connect(ip, port, username, password, db_index);
    }

    std::expected<void, RedisError> RedisSession::connect(const std::string &ip, int32_t port, const std::string& username, const std::string &password)
    {
        return connect(ip, port, username, password, 0);
    }

    std::expected<void, RedisError> RedisSession::connect(const std::string &ip, int32_t port, const std::string& username, const std::string &password, int32_t db_index)
    {
        return connect(ip, port, username, password, db_index, 2);
    }

    std::expected<void, RedisError> RedisSession::connect(const std::string &ip, int32_t port, const std::string& username, const std::string &password, int32_t db_index, int version)
    {
        std::string host = ip;
        if (host == "localhost") {
            host = "127.0.0.1";
        }

        // 连接到Redis服务器
        const uint32_t timeout_ms = m_config.connect_timeout_ms;
        auto connect_result = m_connection->connect(host, port, timeout_ms, m_config.tcp_no_delay);
        if (!connect_result) {
            REDIS_LOG_ERROR("[client]", "[Redis connect to {}:{} failed, error is {}]", host.c_str(), port, connect_result.error().message());
            return connect_result;
        }

        REDIS_LOG_INFO("[client]", "[Redis connect to {}:{}]", host.c_str(), port);

        // Authentication
        if (!password.empty()) {
            std::vector<std::string> auth_cmd;
            if (version == 3) {
                auth_cmd = {"HELLO", "3", "AUTH", username.empty() ? "default" : username, password};
            } else {
                if (username.empty()) {
                    auth_cmd = {"AUTH", password};
                } else {
                    auth_cmd = {"AUTH", username, password};
                }
            }

            auto auth_reply = redis_command(m_encoder.encode_command(auth_cmd));
            if (!auth_reply || auth_reply->is_error()) {
                std::string error_msg = auth_reply ? auth_reply->to_error() : "Authentication failure";
                REDIS_LOG_ERROR("[client]", "[Authentication failure, error is {}]", error_msg);
                disconnect();
                return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_AUTH_ERROR, error_msg));
            }
            REDIS_LOG_INFO("[client]", "[Authentication success]");
        }

        // 选择数据库
        if (db_index != 0) {
            auto select_reply = select_db(db_index);
            if (!select_reply) {
                auto error = select_reply.error();
                return std::unexpected(std::move(error));
            }
            if (select_reply->is_null() || !select_reply->is_status()) {
                return std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_PARSE_ERROR,
                    "Unexpected SELECT response"));
            }
        }

        return {};
    }

    std::expected<void, RedisError> RedisSession::disconnect()
    {
        if (m_connection) {
            m_connection->disconnect();
            return {};
        }
        return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_FREE_REDISOBJ_ERROR, "Redis release connection failed"));
    }

    std::expected<RedisValue, RedisError> RedisSession::select_db(int32_t db_index)
    {
        std::string cmd = m_encoder.encode_command({"SELECT", std::to_string(db_index)});
        return redis_command(cmd);
    }

    std::expected<RedisValue, RedisError> RedisSession::flush_db()
    {
        return redis_command(m_encoder.encode_command({"FLUSHDB"}));
    }

    std::expected<RedisValue, RedisError> RedisSession::switch_version(int version)
    {
        std::string cmd = m_encoder.encode_command({"HELLO", std::to_string(version)});
        return redis_command(cmd);
    }

    std::expected<RedisValue, RedisError> RedisSession::exist(const std::string &key)
    {
        return redis_command(m_encoder.encode_command({"EXISTS", key}));
    }

    std::expected<RedisValue, RedisError> RedisSession::get(const std::string& key)
    {
        return redis_command(m_encoder.encode_command({"GET", key}));
    }

    std::expected<RedisValue, RedisError> RedisSession::set(const std::string &key, const std::string &value)
    {
        return redis_command(m_encoder.encode_command({"SET", key, value}));
    }

    std::expected<RedisValue, RedisError> RedisSession::del(const std::string &key)
    {
        return redis_command(m_encoder.encode_command({"DEL", key}));
    }

    std::expected<RedisValue, RedisError> RedisSession::set_ex(const std::string &key, int64_t seconds, const std::string &value)
    {
        return redis_command(m_encoder.encode_command({"SETEX", key, std::to_string(seconds), value}));
    }

    std::expected<RedisValue, RedisError> RedisSession::pset_ex(const std::string &key, int64_t milliseconds, const std::string &value)
    {
        return redis_command(m_encoder.encode_command({"PSETEX", key, std::to_string(milliseconds), value}));
    }

    std::expected<RedisValue, RedisError> RedisSession::incr(const std::string& key)
    {
        return redis_command(m_encoder.encode_command({"INCR", key}));
    }

    std::expected<RedisValue, RedisError> RedisSession::incr_by(std::string key, int64_t value)
    {
        return redis_command(m_encoder.encode_command({"INCRBY", key, std::to_string(value)}));
    }

    std::expected<RedisValue, RedisError> RedisSession::decr(const std::string& key)
    {
        return redis_command(m_encoder.encode_command({"DECR", key}));
    }

    std::expected<RedisValue, RedisError> RedisSession::hget(const std::string& key, const std::string& field)
    {
        return redis_command(m_encoder.encode_command({"HGET", key, field}));
    }

    std::expected<RedisValue, RedisError> RedisSession::hset(const std::string &key, const std::string &field, const std::string &value)
    {
        return redis_command(m_encoder.encode_command({"HSET", key, field, value}));
    }

    std::expected<RedisValue, RedisError> RedisSession::hget_all(const std::string& key)
    {
        return redis_command(m_encoder.encode_command({"HGETALL", key}));
    }

    std::expected<RedisValue, RedisError> RedisSession::hincr_by(const std::string& key, std::string field, int64_t value)
    {
        return redis_command(m_encoder.encode_command({"HINCRBY", key, field, std::to_string(value)}));
    }

    std::expected<RedisValue, RedisError> RedisSession::l_len(const std::string& key)
    {
        return redis_command(m_encoder.encode_command({"LLEN", key}));
    }

    std::expected<RedisValue, RedisError> RedisSession::lrange(const std::string &key, int64_t start, int64_t end)
    {
        return redis_command(m_encoder.encode_command({"LRANGE", key, std::to_string(start), std::to_string(end)}));
    }

    std::expected<RedisValue, RedisError> RedisSession::lrem(const std::string &key, const std::string& value, int64_t count)
    {
        return redis_command(m_encoder.encode_command({"LREM", key, std::to_string(count), value}));
    }

    std::expected<RedisValue, RedisError> RedisSession::smembers(const std::string &key)
    {
        return redis_command(m_encoder.encode_command({"SMEMBERS", key}));
    }

    std::expected<RedisValue, RedisError> RedisSession::smove(const std::string &source, const std::string &destination, const std::string &member)
    {
        return redis_command(m_encoder.encode_command({"SMOVE", source, destination, member}));
    }

    std::expected<RedisValue, RedisError> RedisSession::scard(const std::string &key)
    {
        return redis_command(m_encoder.encode_command({"SCARD", key}));
    }

    std::expected<RedisValue, RedisError> RedisSession::zrange(const std::string& key, uint32_t beg, uint32_t end)
    {
        return redis_command(m_encoder.encode_command({"ZRANGE", key, std::to_string(beg), std::to_string(end)}));
    }

    std::expected<RedisValue, RedisError> RedisSession::zscore(const std::string &key, const std::string &member)
    {
        return redis_command(m_encoder.encode_command({"ZSCORE", key, member}));
    }

    std::expected<RedisValue, RedisError> RedisSession::redis_command(const std::string &encoded_cmd)
    {
        REDIS_LOG_INFO("[client]", "[redisCommand]");

        if (!m_connection || !m_connection->is_connected()) {
            REDIS_LOG_ERROR("[client]", "[redisCommand failed, not connected]");
            return std::unexpected(RedisError(RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR, "Not connected"));
        }

        auto reply_result = m_connection->execute(encoded_cmd);
        if (!reply_result) {
            REDIS_LOG_ERROR("[client]", "[redisCommand failed, error is {}]", reply_result.error().message());
            return std::unexpected(reply_result.error());
        }

        return RedisValue(std::move(reply_result.value()));
    }

    RedisSession::~RedisSession()
    {
        if (m_connection) {
            m_connection->disconnect();
        }
    }
}
