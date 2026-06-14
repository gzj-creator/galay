#include <chrono>
#include <string>

#include <etcd/base/etcd_config.h>
#include <mongo/base/mongo_config.h>
#include <mysql/base/mysql_config.h>
#include <redis/base/redis_config.h>

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
