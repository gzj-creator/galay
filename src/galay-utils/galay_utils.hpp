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
#include "galay-utils/common/defn.hpp"
/// 类型名称工具
#include "galay-utils/core/type_name.hpp"

/// 字符串工具
#include "galay-utils/core/string.hpp"

/// 随机数生成
#include "galay-utils/core/random.hpp"

/// 系统工具
#include "galay-utils/process/system.hpp"

/// 时间工具
#include "galay-utils/core/time.hpp"

/// 堆栈跟踪
#include "galay-utils/process/backtrace.hpp"

/// 信号处理
#include "galay-utils/process/signal.hpp"

/// 对象池
#include "galay-utils/tool/pool.hpp"

/// LRU 缓存
#include "galay-utils/cache/lru_cache.hpp"

/// 字节容器
#include "galay-utils/cache/bytes.hpp"

/// 字节队列视图
#include "galay-utils/cache/byte_queue_view.hpp"

/// 环形缓冲区
#include "galay-utils/cache/ring_buffer.hpp"

/// 线程池
#include "galay-utils/tool/thread.hpp"

/// 熔断器
#include "galay-utils/tool/circuit_breaker.hpp"

/// 一致性哈希
#include "galay-utils/algorithm/consistent_hash.hpp"

/// 布隆过滤器
#include "galay-utils/algorithm/bloom_filter.hpp"

/// 字典树
#include "galay-utils/algorithm/trie.hpp"

/// 哈夫曼编码
#include "galay-utils/algorithm/huffman.hpp"

/// 多版本并发控制
#include "galay-utils/algorithm/mvcc.hpp"

/// 命令行参数解析
#include "galay-utils/app/app.hpp"

/// 配置文件解析
#include "galay-utils/config/parser_manager.hpp"

/// 进程管理
#include "galay-utils/process/process.hpp"

/// 负载均衡
#include "galay-utils/tool/balancer.hpp"

/// Base64 编解码
#include "galay-utils/encoding/base64.hpp"
/// SHA1 哈希
#include "galay-utils/crypto/sha1.hpp"
/// MD5 哈希
#include "galay-utils/crypto/md5.hpp"
/// MurmurHash3 哈希
#include "galay-utils/crypto/murmur_hash3.hpp"
/// 盐值生成
#include "galay-utils/crypto/salt.hpp"
/// HMAC-SHA256
#include "galay-utils/crypto/hmac.hpp"
/// PBKDF2-HMAC-SHA256
#include "galay-utils/crypto/pbkdf2.hpp"

#endif // GALAY_UTILS_HPP
