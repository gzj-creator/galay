#ifndef GALAY_REDIS_SYNC_SESSION_INL
#define GALAY_REDIS_SYNC_SESSION_INL

#include "galay-redis/sync/redis_session.h"

namespace galay::redis
{

template <KVPair... KV>
inline std::expected<RedisValue, RedisError> RedisSession::mset(KV... pairs)
{
    std::vector<std::string> cmd_parts;
    cmd_parts.push_back("MSET");
    ((cmd_parts.push_back(std::get<0>(pairs)), cmd_parts.push_back(std::get<1>(pairs))), ...);
    auto reply = redisCommand(m_encoder.encodeCommand(cmd_parts));
    return reply;
}

template<KeyType... Key>
inline std::expected<RedisValue, RedisError> RedisSession::mget(Key... keys)
{
    std::vector<std::string> cmd_parts;
    cmd_parts.push_back("MGET");
    ((cmd_parts.push_back(keys)), ...);
    auto reply = redisCommand(m_encoder.encodeCommand(cmd_parts));
    return reply;
}

template <KeyType... Key>
inline std::expected<RedisValue, RedisError> RedisSession::hdel(const std::string& key, Key... fields)
{
    std::vector<std::string> cmd_parts;
    cmd_parts.push_back("HDEL");
    cmd_parts.push_back(key);
    ((cmd_parts.push_back(fields)), ...);
    auto reply = redisCommand(m_encoder.encodeCommand(cmd_parts));
    return reply;
}

template <KeyType... Key>
inline std::expected<RedisValue, RedisError> RedisSession::hmget(const std::string &key, Key... field)
{
    std::vector<std::string> cmd_parts;
    cmd_parts.push_back("HMGET");
    cmd_parts.push_back(key);
    ((cmd_parts.push_back(field)), ...);
    auto reply = redisCommand(m_encoder.encodeCommand(cmd_parts));
    return reply;
}

template <KVPair... KV>
inline std::expected<RedisValue, RedisError> RedisSession::hmset(const std::string& key, KV... pairs)
{
    std::vector<std::string> cmd_parts;
    cmd_parts.push_back("HMSET");
    cmd_parts.push_back(key);
    ((cmd_parts.push_back(std::get<0>(pairs)), cmd_parts.push_back(std::get<1>(pairs))), ...);
    auto reply = redisCommand(m_encoder.encodeCommand(cmd_parts));
    return reply;
}

template <ValType... Val>
inline std::expected<RedisValue, RedisError> RedisSession::lpush(const std::string& key, Val... values)
{
    std::vector<std::string> cmd_parts;
    cmd_parts.push_back("LPUSH");
    cmd_parts.push_back(key);
    ((cmd_parts.push_back(values)), ...);
    auto reply = redisCommand(m_encoder.encodeCommand(cmd_parts));
    return reply;
}

template <ValType... Val>
inline std::expected<RedisValue, RedisError> RedisSession::rpush(const std::string& key, Val... values)
{
    std::vector<std::string> cmd_parts;
    cmd_parts.push_back("RPUSH");
    cmd_parts.push_back(key);
    ((cmd_parts.push_back(values)), ...);
    auto reply = redisCommand(m_encoder.encodeCommand(cmd_parts));
    return reply;
}

template <ValType... Val>
inline std::expected<RedisValue, RedisError> RedisSession::sadd(const std::string &key, Val... members)
{
    std::vector<std::string> cmd_parts;
    cmd_parts.push_back("SADD");
    cmd_parts.push_back(key);
    ((cmd_parts.push_back(members)), ...);
    auto reply = redisCommand(m_encoder.encodeCommand(cmd_parts));
    return reply;
}

template <ValType... Val>
inline std::expected<RedisValue, RedisError> RedisSession::srem(const std::string &key, Val... members)
{
    std::vector<std::string> cmd_parts;
    cmd_parts.push_back("SREM");
    cmd_parts.push_back(key);
    ((cmd_parts.push_back(members)), ...);
    auto reply = redisCommand(m_encoder.encodeCommand(cmd_parts));
    return reply;
}

template <KeyType... Key>
inline std::expected<RedisValue, RedisError> RedisSession::sinter(Key... keys)
{
    std::vector<std::string> cmd_parts;
    cmd_parts.push_back("SINTER");
    ((cmd_parts.push_back(keys)), ...);
    auto reply = redisCommand(m_encoder.encodeCommand(cmd_parts));
    return reply;
}

template <KeyType... Key>
inline std::expected<RedisValue, RedisError> RedisSession::sunion(Key... keys)
{
    std::vector<std::string> cmd_parts;
    cmd_parts.push_back("SUNION");
    ((cmd_parts.push_back(keys)), ...);
    auto reply = redisCommand(m_encoder.encodeCommand(cmd_parts));
    return reply;
}

template <ScoreValType... KV>
inline std::expected<RedisValue, RedisError> RedisSession::zadd(const std::string &key, KV... values)
{
    std::vector<std::string> cmd_parts;
    cmd_parts.push_back("ZADD");
    cmd_parts.push_back(key);
    ((cmd_parts.push_back(std::to_string(std::get<0>(values))), cmd_parts.push_back(std::get<1>(values))), ...);
    auto reply = redisCommand(m_encoder.encodeCommand(cmd_parts));
    return reply;
}

template <KeyType... Key>
inline std::expected<RedisValue, RedisError> RedisSession::zrem(const std::string &key, Key... members)
{
    std::vector<std::string> cmd_parts;
    cmd_parts.push_back("ZREM");
    cmd_parts.push_back(key);
    ((cmd_parts.push_back(members)), ...);
    auto reply = redisCommand(m_encoder.encodeCommand(cmd_parts));
    return reply;
}

}


#endif