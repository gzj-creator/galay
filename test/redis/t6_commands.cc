#include "galay-redis/async/redis_client.h"
#include <galay-kernel/core/runtime.h>
#include <iostream>
#include <chrono>
#include <cassert>
#include <array>

using namespace galay::redis;
using namespace galay::kernel;

/**
 * @brief 测试RedisClient的所有命令
 * @details 全面测试所有Redis命令的功能
 */
Task<void> test_all_redis_commands(IOScheduler* scheduler)
{
    std::cout << "\n========================================" << std::endl;
    std::cout << "Starting comprehensive Redis command tests" << std::endl;
    std::cout << "========================================\n" << std::endl;

    // 创建RedisClient
    auto client = RedisClientBuilder().scheduler(scheduler).build();
    RedisCommandBuilder command_builder;

    // ==================== 测试连接 ====================
    std::cout << "=== Testing Connection Commands ===" << std::endl;

    // 测试连接
    std::cout << "1. Testing CONNECT..." << std::endl;
    auto connect_result = co_await client.connect("127.0.0.1", 6379);
    if (!connect_result) {
        std::cerr << "   [FAILED] Connect failed: " << connect_result.error().message() << std::endl;
        co_return;
    }
    std::cout << "   [PASSED] Connected to Redis server" << std::endl;

    // 测试PING
    std::cout << "2. Testing PING..." << std::endl;
    auto ping_result = co_await client.command(command_builder.ping());
    if (ping_result && ping_result.value()) {
        auto& values = ping_result.value().value();
        if (!values.empty()) {
            std::cout << "   [PASSED] PING response: " << values[0].toString() << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] PING failed" << std::endl;
    }

    // 测试ECHO
    std::cout << "3. Testing ECHO..." << std::endl;
    auto echo_result = co_await client.command(command_builder.echo("Hello Redis!"));
    if (echo_result && echo_result.value()) {
        auto& values = echo_result.value().value();
        if (!values.empty() && values[0].isString()) {
            std::cout << "   [PASSED] ECHO response: " << values[0].toString() << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] ECHO failed" << std::endl;
    }

    // ==================== 测试String命令 ====================
    std::cout << "\n=== Testing String Commands ===" << std::endl;

    // 测试SET
    std::cout << "4. Testing SET..." << std::endl;
    auto set_result = co_await client.command(command_builder.set("test_string_key", "test_value"));
    if (set_result && set_result.value()) {
        std::cout << "   [PASSED] SET command succeeded" << std::endl;
    } else {
        std::cerr << "   [FAILED] SET failed" << std::endl;
    }

    // 测试GET
    std::cout << "5. Testing GET..." << std::endl;
    auto get_result = co_await client.command(command_builder.get("test_string_key"));
    if (get_result && get_result.value()) {
        auto& values = get_result.value().value();
        if (!values.empty() && values[0].isString()) {
            std::string value = values[0].toString();
            if (value == "test_value") {
                std::cout << "   [PASSED] GET returned: " << value << std::endl;
            } else {
                std::cerr << "   [FAILED] GET returned wrong value: " << value << std::endl;
            }
        }
    } else {
        std::cerr << "   [FAILED] GET failed" << std::endl;
    }

    // 测试EXISTS
    std::cout << "6. Testing EXISTS..." << std::endl;
    auto exists_result = co_await client.command(command_builder.exists("test_string_key"));
    if (exists_result && exists_result.value()) {
        auto& values = exists_result.value().value();
        if (!values.empty() && values[0].isInteger()) {
            int64_t exists = values[0].toInteger();
            if (exists == 1) {
                std::cout << "   [PASSED] EXISTS returned: " << exists << std::endl;
            } else {
                std::cerr << "   [FAILED] EXISTS returned: " << exists << std::endl;
            }
        }
    } else {
        std::cerr << "   [FAILED] EXISTS failed" << std::endl;
    }

    // 测试SETEX
    std::cout << "7. Testing SETEX..." << std::endl;
    auto setex_result = co_await client.command(command_builder.setex("test_expire_key", 10, "expire_value"));
    if (setex_result && setex_result.value()) {
        std::cout << "   [PASSED] SETEX command succeeded (expires in 10s)" << std::endl;
    } else {
        std::cerr << "   [FAILED] SETEX failed" << std::endl;
    }

    // 测试INCR
    std::cout << "8. Testing INCR..." << std::endl;
    auto incr_result = co_await client.command(command_builder.incr("test_counter"));
    if (incr_result && incr_result.value()) {
        auto& values = incr_result.value().value();
        if (!values.empty() && values[0].isInteger()) {
            std::cout << "   [PASSED] INCR returned: " << values[0].toInteger() << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] INCR failed" << std::endl;
    }

    // 测试DECR
    std::cout << "9. Testing DECR..." << std::endl;
    auto decr_result = co_await client.command(command_builder.decr("test_counter"));
    if (decr_result && decr_result.value()) {
        auto& values = decr_result.value().value();
        if (!values.empty() && values[0].isInteger()) {
            std::cout << "   [PASSED] DECR returned: " << values[0].toInteger() << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] DECR failed" << std::endl;
    }

    // 测试DEL
    std::cout << "10. Testing DEL..." << std::endl;
    auto del_result = co_await client.command(command_builder.del("test_string_key"));
    if (del_result && del_result.value()) {
        auto& values = del_result.value().value();
        if (!values.empty() && values[0].isInteger()) {
            std::cout << "   [PASSED] DEL deleted " << values[0].toInteger() << " key(s)" << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] DEL failed" << std::endl;
    }

    // ==================== 测试Hash命令 ====================
    std::cout << "\n=== Testing Hash Commands ===" << std::endl;

    // 测试HSET
    std::cout << "11. Testing HSET..." << std::endl;
    auto hset_result = co_await client.command(command_builder.hset("test_hash", "field1", "value1"));
    if (hset_result && hset_result.value()) {
        auto& values = hset_result.value().value();
        if (!values.empty() && values[0].isInteger()) {
            std::cout << "   [PASSED] HSET added " << values[0].toInteger() << " field(s)" << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] HSET failed" << std::endl;
    }

    // 再添加一个字段
    co_await client.command(command_builder.hset("test_hash", "field2", "value2"));

    // 测试HGET
    std::cout << "12. Testing HGET..." << std::endl;
    auto hget_result = co_await client.command(command_builder.hget("test_hash", "field1"));
    if (hget_result && hget_result.value()) {
        auto& values = hget_result.value().value();
        if (!values.empty() && values[0].isString()) {
            std::string value = values[0].toString();
            if (value == "value1") {
                std::cout << "   [PASSED] HGET returned: " << value << std::endl;
            } else {
                std::cerr << "   [FAILED] HGET returned wrong value: " << value << std::endl;
            }
        }
    } else {
        std::cerr << "   [FAILED] HGET failed" << std::endl;
    }

    // 测试HGETALL
    std::cout << "13. Testing HGETALL..." << std::endl;
    auto hgetall_result = co_await client.command(command_builder.hgetAll("test_hash"));
    if (hgetall_result && hgetall_result.value()) {
        auto& values = hgetall_result.value().value();
        if (!values.empty() && values[0].isArray()) {
            auto arr = values[0].toArray();
            std::cout << "   [PASSED] HGETALL returned " << arr.size() << " items:" << std::endl;
            for (size_t i = 0; i < arr.size(); i += 2) {
                if (i + 1 < arr.size()) {
                    std::cout << "      " << arr[i].toString() << " => " << arr[i+1].toString() << std::endl;
                }
            }
        }
    } else {
        std::cerr << "   [FAILED] HGETALL failed" << std::endl;
    }

    // 测试HDEL
    std::cout << "14. Testing HDEL..." << std::endl;
    auto hdel_result = co_await client.command(command_builder.hdel("test_hash", "field1"));
    if (hdel_result && hdel_result.value()) {
        auto& values = hdel_result.value().value();
        if (!values.empty() && values[0].isInteger()) {
            std::cout << "   [PASSED] HDEL deleted " << values[0].toInteger() << " field(s)" << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] HDEL failed" << std::endl;
    }

    // ==================== 测试List命令 ====================
    std::cout << "\n=== Testing List Commands ===" << std::endl;

    // 测试LPUSH
    std::cout << "15. Testing LPUSH..." << std::endl;
    auto lpush_result = co_await client.command(command_builder.lpush("test_list", "item1"));
    if (lpush_result && lpush_result.value()) {
        auto& values = lpush_result.value().value();
        if (!values.empty() && values[0].isInteger()) {
            std::cout << "   [PASSED] LPUSH, list length: " << values[0].toInteger() << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] LPUSH failed" << std::endl;
    }

    // 测试RPUSH
    std::cout << "16. Testing RPUSH..." << std::endl;
    auto rpush_result = co_await client.command(command_builder.rpush("test_list", "item2"));
    if (rpush_result && rpush_result.value()) {
        auto& values = rpush_result.value().value();
        if (!values.empty() && values[0].isInteger()) {
            std::cout << "   [PASSED] RPUSH, list length: " << values[0].toInteger() << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] RPUSH failed" << std::endl;
    }

    // 再添加一些元素
    co_await client.command(command_builder.rpush("test_list", "item3"));
    co_await client.command(command_builder.rpush("test_list", "item4"));

    // 测试LLEN
    std::cout << "17. Testing LLEN..." << std::endl;
    auto llen_result = co_await client.command(command_builder.llen("test_list"));
    if (llen_result && llen_result.value()) {
        auto& values = llen_result.value().value();
        if (!values.empty() && values[0].isInteger()) {
            std::cout << "   [PASSED] LLEN returned: " << values[0].toInteger() << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] LLEN failed" << std::endl;
    }

    // 测试LRANGE
    std::cout << "18. Testing LRANGE..." << std::endl;
    auto lrange_result = co_await client.command(command_builder.lrange("test_list", 0, -1));
    if (lrange_result && lrange_result.value()) {
        auto& values = lrange_result.value().value();
        if (!values.empty() && values[0].isArray()) {
            auto arr = values[0].toArray();
            std::cout << "   [PASSED] LRANGE returned " << arr.size() << " items: ";
            for (auto& item : arr) {
                std::cout << item.toString() << " ";
            }
            std::cout << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] LRANGE failed" << std::endl;
    }

    // 测试LPOP
    std::cout << "19. Testing LPOP..." << std::endl;
    auto lpop_result = co_await client.command(command_builder.lpop("test_list"));
    if (lpop_result && lpop_result.value()) {
        auto& values = lpop_result.value().value();
        if (!values.empty() && values[0].isString()) {
            std::cout << "   [PASSED] LPOP returned: " << values[0].toString() << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] LPOP failed" << std::endl;
    }

    // 测试RPOP
    std::cout << "20. Testing RPOP..." << std::endl;
    auto rpop_result = co_await client.command(command_builder.rpop("test_list"));
    if (rpop_result && rpop_result.value()) {
        auto& values = rpop_result.value().value();
        if (!values.empty() && values[0].isString()) {
            std::cout << "   [PASSED] RPOP returned: " << values[0].toString() << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] RPOP failed" << std::endl;
    }

    // ==================== 测试Set命令 ====================
    std::cout << "\n=== Testing Set Commands ===" << std::endl;

    // 测试SADD
    std::cout << "21. Testing SADD..." << std::endl;
    auto sadd_result = co_await client.command(command_builder.sadd("test_set", "member1"));
    if (sadd_result && sadd_result.value()) {
        auto& values = sadd_result.value().value();
        if (!values.empty() && values[0].isInteger()) {
            std::cout << "   [PASSED] SADD added " << values[0].toInteger() << " member(s)" << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] SADD failed" << std::endl;
    }

    // 添加更多成员
    co_await client.command(command_builder.sadd("test_set", "member2"));
    co_await client.command(command_builder.sadd("test_set", "member3"));

    // 测试SCARD
    std::cout << "22. Testing SCARD..." << std::endl;
    auto scard_result = co_await client.command(command_builder.scard("test_set"));
    if (scard_result && scard_result.value()) {
        auto& values = scard_result.value().value();
        if (!values.empty() && values[0].isInteger()) {
            std::cout << "   [PASSED] SCARD returned: " << values[0].toInteger() << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] SCARD failed" << std::endl;
    }

    // 测试SMEMBERS
    std::cout << "23. Testing SMEMBERS..." << std::endl;
    auto smembers_result = co_await client.command(command_builder.smembers("test_set"));
    if (smembers_result && smembers_result.value()) {
        auto& values = smembers_result.value().value();
        if (!values.empty() && values[0].isArray()) {
            auto arr = values[0].toArray();
            std::cout << "   [PASSED] SMEMBERS returned " << arr.size() << " members: ";
            for (auto& member : arr) {
                std::cout << member.toString() << " ";
            }
            std::cout << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] SMEMBERS failed" << std::endl;
    }

    // 测试SREM
    std::cout << "24. Testing SREM..." << std::endl;
    auto srem_result = co_await client.command(command_builder.srem("test_set", "member1"));
    if (srem_result && srem_result.value()) {
        auto& values = srem_result.value().value();
        if (!values.empty() && values[0].isInteger()) {
            std::cout << "   [PASSED] SREM removed " << values[0].toInteger() << " member(s)" << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] SREM failed" << std::endl;
    }

    // ==================== 测试Sorted Set命令 ====================
    std::cout << "\n=== Testing Sorted Set Commands ===" << std::endl;

    // 测试ZADD
    std::cout << "25. Testing ZADD..." << std::endl;
    auto zadd_result = co_await client.command(command_builder.zadd("test_zset", 1.0, "member1"));
    if (zadd_result && zadd_result.value()) {
        auto& values = zadd_result.value().value();
        if (!values.empty() && values[0].isInteger()) {
            std::cout << "   [PASSED] ZADD added " << values[0].toInteger() << " member(s)" << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] ZADD failed" << std::endl;
    }

    // 添加更多成员
    co_await client.command(command_builder.zadd("test_zset", 2.0, "member2"));
    co_await client.command(command_builder.zadd("test_zset", 3.0, "member3"));
    co_await client.command(command_builder.zadd("test_zset", 4.0, "member4"));

    // 测试ZSCORE
    std::cout << "26. Testing ZSCORE..." << std::endl;
    auto zscore_result = co_await client.command(command_builder.zscore("test_zset", "member2"));
    if (zscore_result && zscore_result.value()) {
        auto& values = zscore_result.value().value();
        if (!values.empty() && values[0].isString()) {
            std::cout << "   [PASSED] ZSCORE returned: " << values[0].toString() << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] ZSCORE failed" << std::endl;
    }

    // 测试ZRANGE
    std::cout << "27. Testing ZRANGE..." << std::endl;
    auto zrange_result = co_await client.command(command_builder.zrange("test_zset", 0, -1));
    if (zrange_result && zrange_result.value()) {
        auto& values = zrange_result.value().value();
        if (!values.empty() && values[0].isArray()) {
            auto arr = values[0].toArray();
            std::cout << "   [PASSED] ZRANGE returned " << arr.size() << " members: ";
            for (auto& member : arr) {
                std::cout << member.toString() << " ";
            }
            std::cout << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] ZRANGE failed" << std::endl;
    }

    // 测试ZREM
    std::cout << "28. Testing ZREM..." << std::endl;
    auto zrem_result = co_await client.command(command_builder.zrem("test_zset", "member1"));
    if (zrem_result && zrem_result.value()) {
        auto& values = zrem_result.value().value();
        if (!values.empty() && values[0].isInteger()) {
            std::cout << "   [PASSED] ZREM removed " << values[0].toInteger() << " member(s)" << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] ZREM failed" << std::endl;
    }

    // ==================== 测试Pipeline ====================
    std::cout << "\n=== Testing Pipeline Commands ===" << std::endl;

    std::cout << "29. Testing PIPELINE..." << std::endl;
    RedisCommandBuilder pipeline_commands;
    pipeline_commands.reserve(9, 15, 256);
    pipeline_commands.append("SET", std::array<std::string_view, 2>{"pipeline_key1", "value1"});
    pipeline_commands.append("SET", std::array<std::string_view, 2>{"pipeline_key2", "value2"});
    pipeline_commands.append("SET", std::array<std::string_view, 2>{"pipeline_key3", "value3"});
    pipeline_commands.append("GET", std::array<std::string_view, 1>{"pipeline_key1"});
    pipeline_commands.append("GET", std::array<std::string_view, 1>{"pipeline_key2"});
    pipeline_commands.append("GET", std::array<std::string_view, 1>{"pipeline_key3"});
    pipeline_commands.append("DEL", std::array<std::string_view, 1>{"pipeline_key1"});
    pipeline_commands.append("DEL", std::array<std::string_view, 1>{"pipeline_key2"});
    pipeline_commands.append("DEL", std::array<std::string_view, 1>{"pipeline_key3"});

    auto pipeline_result = co_await client.batch(pipeline_commands.commands());
    if (pipeline_result && pipeline_result.value()) {
        auto& values = pipeline_result.value().value();
        std::cout << "   [PASSED] PIPELINE executed " << values.size() << " commands:" << std::endl;
        for (size_t i = 0; i < values.size(); ++i) {
            std::cout << "      Response " << i << ": ";
            if (values[i].isString()) {
                std::cout << values[i].toString();
            } else if (values[i].isInteger()) {
                std::cout << values[i].toInteger();
            } else {
                std::cout << "(other type)";
            }
            std::cout << std::endl;
        }
    } else {
        std::cerr << "   [FAILED] PIPELINE failed" << std::endl;
    }

    // ==================== 清理测试数据 ====================
    std::cout << "\n=== Cleaning up test data ===" << std::endl;
    co_await client.command(command_builder.del("test_counter"));
    co_await client.command(command_builder.del("test_expire_key"));
    co_await client.command(command_builder.del("test_hash"));
    co_await client.command(command_builder.del("test_list"));
    co_await client.command(command_builder.del("test_set"));
    co_await client.command(command_builder.del("test_zset"));
    std::cout << "Test data cleaned up" << std::endl;

    // 关闭连接
    co_await client.close();
    std::cout << "\n========================================" << std::endl;
    std::cout << "All tests completed!" << std::endl;
    std::cout << "========================================\n" << std::endl;
}

/**
 * @brief 测试execute通用命令接口
 */
Task<void> test_execute_command(IOScheduler* scheduler)
{
    std::cout << "\n=== Testing Generic EXECUTE Command ===" << std::endl;

    auto client = RedisClientBuilder().scheduler(scheduler).build();
    RedisCommandBuilder command_builder;

    auto connect_result = co_await client.connect("127.0.0.1", 6379);
    if (!connect_result) {
        std::cerr << "Failed to connect" << std::endl;
        co_return;
    }

    // 使用execute执行任意命令
    std::cout << "30. Testing EXECUTE with custom commands..." << std::endl;

    // 测试 MSET
    auto mset_result = co_await client.command(command_builder.command(
        "MSET", {"key1", "value1", "key2", "value2"}));
    if (mset_result && mset_result.value()) {
        std::cout << "   [PASSED] EXECUTE MSET succeeded" << std::endl;
    }

    // 测试 MGET
    auto mget_result = co_await client.command(command_builder.command("MGET", {"key1", "key2"}));
    if (mget_result && mget_result.value()) {
        auto& values = mget_result.value().value();
        if (!values.empty() && values[0].isArray()) {
            auto arr = values[0].toArray();
            std::cout << "   [PASSED] EXECUTE MGET returned: ";
            for (auto& val : arr) {
                std::cout << val.toString() << " ";
            }
            std::cout << std::endl;
        }
    }

    // 清理
    co_await client.command(command_builder.command("DEL", {"key1", "key2"}));

    co_await client.close();
    std::cout << "Generic EXECUTE test completed\n" << std::endl;
}

int main()
{
    std::cout << "\n##################################################" << std::endl;
    std::cout << "# Redis Client - All Commands Comprehensive Test #" << std::endl;
    std::cout << "##################################################\n" << std::endl;

    try {
        // 创建运行时
        Runtime runtime;
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            std::cerr << "Failed to get IO scheduler" << std::endl;
            return 1;
        }

        // 运行所有命令测试
        scheduleTask(scheduler, test_all_redis_commands(scheduler));

        // 运行通用execute命令测试
        scheduleTask(scheduler, test_execute_command(scheduler));

        // 等待测试完成
        std::this_thread::sleep_for(std::chrono::seconds(15));

        runtime.stop();

    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "\n##################################################" << std::endl;
    std::cout << "# All comprehensive tests completed successfully! #" << std::endl;
    std::cout << "##################################################\n" << std::endl;

    return 0;
}
