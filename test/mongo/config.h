#ifndef GALAY_MONGO_TEST_CONFIG_H
#define GALAY_MONGO_TEST_CONFIG_H

#include "mongo/base/mongo_config.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace mongo_test
{

struct MongoTestConfig
{
    std::string host = "127.0.0.1";
    uint16_t port = 27017;
    std::string database = "test";
    std::string username;
    std::string password;
    std::string auth_database = "admin";
    std::string hello_database = "admin";
    bool tcp_nodelay = true;
    size_t recv_buffer_size = 16384;
};

inline std::string envOrDefault(const char* key, std::string fallback)
{
    const char* value = std::getenv(key);
    return value != nullptr ? std::string(value) : std::move(fallback);
}

inline uint16_t envPortOrDefault(const char* key, uint16_t fallback)
{
    const char* value = std::getenv(key);
    if (value == nullptr) {
        return fallback;
    }

    try {
        const int v = std::stoi(value);
        if (v <= 0 || v > 65535) {
            return fallback;
        }
        return static_cast<uint16_t>(v);
    } catch (...) {
        return fallback;
    }
}

inline MongoTestConfig loadMongoTestConfig()
{
    MongoTestConfig cfg;
    cfg.host = envOrDefault("GALAY_MONGO_HOST", cfg.host);
    cfg.port = envPortOrDefault("GALAY_MONGO_PORT", cfg.port);
    cfg.database = envOrDefault("GALAY_MONGO_DB", cfg.database);
    cfg.username = envOrDefault("GALAY_MONGO_USER", "");
    cfg.password = envOrDefault("GALAY_MONGO_PASSWORD", "");
    cfg.auth_database = envOrDefault("GALAY_MONGO_AUTH_DB", cfg.auth_database);
    cfg.hello_database = envOrDefault("GALAY_MONGO_HELLO_DB", cfg.hello_database);
    cfg.tcp_nodelay = envOrDefault("GALAY_MONGO_TCP_NODELAY", "1") != "0";

    const char* recv_buffer_env = std::getenv("GALAY_MONGO_RECV_BUFFER_SIZE");
    if (recv_buffer_env != nullptr) {
        try {
            const unsigned long long parsed = std::stoull(recv_buffer_env);
            if (parsed > 0 &&
                parsed <= static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
                cfg.recv_buffer_size = static_cast<size_t>(parsed);
            }
        } catch (...) {
        }
    }
    return cfg;
}

inline galay::mongo::MongoConfig toMongoConfig(const MongoTestConfig& test_cfg)
{
    galay::mongo::MongoConfig cfg;
    cfg.host = test_cfg.host;
    cfg.port = test_cfg.port;
    cfg.database = test_cfg.database;
    cfg.username = test_cfg.username;
    cfg.password = test_cfg.password;
    cfg.auth_database = test_cfg.auth_database;
    cfg.hello_database = test_cfg.hello_database;
    cfg.tcp_nodelay = test_cfg.tcp_nodelay;
    cfg.recv_buffer_size = test_cfg.recv_buffer_size;
    return cfg;
}

inline galay::mongo::AsyncMongoConfig loadAsyncMongoTestConfig()
{
    galay::mongo::AsyncMongoConfig cfg = galay::mongo::AsyncMongoConfig::noTimeout();

    const char* send_timeout_env = std::getenv("GALAY_MONGO_ASYNC_SEND_TIMEOUT_MS");
    if (send_timeout_env != nullptr) {
        try {
            const int value = std::stoi(send_timeout_env);
            cfg.send_timeout = std::chrono::milliseconds(value);
        } catch (...) {
        }
    }

    const char* recv_timeout_env = std::getenv("GALAY_MONGO_ASYNC_RECV_TIMEOUT_MS");
    if (recv_timeout_env != nullptr) {
        try {
            const int value = std::stoi(recv_timeout_env);
            cfg.recv_timeout = std::chrono::milliseconds(value);
        } catch (...) {
        }
    }

    const char* buffer_size_env = std::getenv("GALAY_MONGO_ASYNC_BUFFER_SIZE");
    if (buffer_size_env != nullptr) {
        try {
            const unsigned long long parsed = std::stoull(buffer_size_env);
            if (parsed > 0 &&
                parsed <= static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
                cfg.buffer_size = static_cast<size_t>(parsed);
            }
        } catch (...) {
        }
    }

    const char* reserve_env = std::getenv("GALAY_MONGO_ASYNC_PIPELINE_RESERVE");
    if (reserve_env != nullptr) {
        try {
            const unsigned long long parsed = std::stoull(reserve_env);
            if (parsed > 0 &&
                parsed <= static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
                cfg.pipeline_reserve_per_command = static_cast<size_t>(parsed);
            }
        } catch (...) {
        }
    }

    return cfg;
}

inline void printMongoTestConfig(const MongoTestConfig& cfg)
{
    std::cout << "Mongo test config: "
              << cfg.host << ":" << cfg.port
              << " db=" << cfg.database
              << " auth_db=" << cfg.auth_database
              << " hello_db=" << cfg.hello_database
              << " tcp_nodelay=" << (cfg.tcp_nodelay ? "1" : "0")
              << " recv_buffer_size=" << cfg.recv_buffer_size
              << " user=" << (cfg.username.empty() ? "<empty>" : cfg.username)
              << std::endl;
}

} // namespace mongo_test

#endif // GALAY_MONGO_TEST_CONFIG_H
