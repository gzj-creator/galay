#include <chrono>
#include <string>

#include <galay/cpp/galay-http/common/macro.hpp>
#include <galay/cpp/galay-utils/common/macro.hpp>
#include <galay/cpp/galay-ws/common/macro.hpp>

#include <galay/cpp/galay-etcd/base/etcd_config.h>
#include <galay/cpp/galay-mongo/base/mongo_config.h>
#include <galay/cpp/galay-mysql/base/mysql_config.h>
#include <galay/cpp/galay-redis/base/redis_config.h>

static_assert(DEFAULT_HTTP_MAX_HEADER_SIZE == 8192);
static_assert(DEFAULT_HTTP_MAX_BODY_SIZE == 1 * 1024 * 1024);
#if defined(__unix__) || defined(__APPLE__)
static_assert(GALAY_UTILS_RING_BUFFER_HAS_IOVEC == 1);
static_assert(GALAY_UTILS_RING_BUFFER_HAS_MMAP == 1);
#else
static_assert(GALAY_UTILS_RING_BUFFER_HAS_IOVEC == 0);
static_assert(GALAY_UTILS_RING_BUFFER_HAS_MMAP == 0);
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
static_assert(GALAY_HTTP_SIMD_X86 == 1);
static_assert(GALAY_WS_SIMD_X86 == 1);
#elif defined(__ARM_NEON) || defined(__aarch64__)
static_assert(GALAY_HTTP_SIMD_NEON == 1);
static_assert(GALAY_WS_SIMD_NEON == 1);
#endif

int main()
{
    galay::etcd::EtcdConfig etcd;
    etcd.endpoint = "http://127.0.0.1:2379";
    etcd.request_timeout = std::chrono::seconds(3);

    galay::mysql::MysqlConfig mysql =
        galay::mysql::MysqlConfig::create("127.0.0.1", 3306, "user", "password", "db");
    galay::mysql::AsyncMysqlConfig mysql_io =
        galay::mysql::AsyncMysqlConfig::withTimeout(std::chrono::seconds(1), std::chrono::seconds(2));

    galay::mongo::MongoConfig mongo =
        galay::mongo::MongoConfig::create("127.0.0.1", 27017, "admin");
    galay::mongo::AsyncMongoConfig mongo_io =
        galay::mongo::AsyncMongoConfig::withTimeout(std::chrono::seconds(1), std::chrono::seconds(2));

    galay::redis::RedisSessionConfig redis_session;
    redis_session.host = "127.0.0.1";
    redis_session.port = 6379;
    redis_session.connect_timeout_ms = 5000;
    galay::redis::AsyncRedisConfig redis_io =
        galay::redis::AsyncRedisConfig::withRecvTimeout(std::chrono::seconds(2));

    return mysql.port == 3306 &&
           mysql_io.isRecvTimeoutEnabled() &&
           mongo.port == 27017 &&
           mongo_io.isSendTimeoutEnabled() &&
           redis_session.port == 6379 &&
           redis_io.isRecvTimeoutEnabled()
        ? 0
        : 1;
}
