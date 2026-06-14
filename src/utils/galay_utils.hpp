/**
 * @file galay_utils.hpp
 * @brief galay-utils 总头文件
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 包含 galay-utils 库的所有模块头文件，提供一站式引用。
 */

#ifndef GALAY_UTILS_HPP
#define GALAY_UTILS_HPP

/// 公共类型定义
#include "utils/common/defn.hpp"
/// 类型名称工具
#include "utils/core/type_name.hpp"

/// 字符串工具
#include "utils/core/string.hpp"

/// 随机数生成
#include "utils/core/random.hpp"

/// 系统工具
#include "utils/process/system.hpp"

/// 时间工具
#include "utils/core/time.hpp"

/// 堆栈跟踪
#include "utils/process/backtrace.hpp"

/// 信号处理
#include "utils/process/signal.hpp"

/// 对象池
#include "utils/tool/pool.hpp"

/// LRU 缓存
#include "utils/cache/lru_cache.hpp"

/// 字节容器
#include "utils/cache/bytes.hpp"

/// 字节队列视图
#include "utils/cache/byte_queue_view.hpp"

/// 环形缓冲区
#include "utils/cache/ring_buffer.hpp"

/// 线程池
#include "utils/tool/thread.hpp"

/// 熔断器
#include "utils/tool/circuit_breaker.hpp"

/// 一致性哈希
#include "utils/algorithm/consistent_hash.hpp"

/// 布隆过滤器
#include "utils/algorithm/bloom_filter.hpp"

/// 字典树
#include "utils/algorithm/trie.hpp"

/// 哈夫曼编码
#include "utils/algorithm/huffman.hpp"

/// 多版本并发控制
#include "utils/algorithm/mvcc.hpp"

/// 命令行参数解析
#include "utils/app/app.hpp"

/// 配置文件解析
#include "utils/config/parser_manager.hpp"

/// 进程管理
#include "utils/process/process.hpp"

/// 负载均衡
#include "utils/tool/balancer.hpp"

/// Base64 编解码
#include "utils/encoding/base64.hpp"
/// SHA1 哈希
#include "utils/crypto/sha1.hpp"
/// MD5 哈希
#include "utils/crypto/md5.hpp"
/// MurmurHash3 哈希
#include "utils/crypto/murmur_hash3.hpp"
/// 盐值生成
#include "utils/crypto/salt.hpp"
/// HMAC-SHA256
#include "utils/crypto/hmac.hpp"
/// PBKDF2-HMAC-SHA256
#include "utils/crypto/pbkdf2.hpp"

#endif // GALAY_UTILS_HPP
