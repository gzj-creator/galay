module;

#include "module_prelude.hpp"

export module galay.utils;

// Match the ABI of the regular galay-utils shared library when this
// transitional interface is consumed by GCC modules.
export extern "C++" {
#include "../common/defn.hpp"
#include "../core/type_name.hpp"

#include "../core/string.hpp"
#include "../core/random.hpp"
#include "../system/system.hpp"
#include "../system/env.hpp"
#include "../core/time.hpp"
#include "../system/backtrace.hpp"
#include "../system/signal.hpp"
#include "../common/pool.hpp"
#include "../cache/lru_cache.hpp"
#include "../buffer/bytes.hpp"
#include "../buffer/byte_queue_view.hpp"
#include "../buffer/ring_buffer.hpp"
#include "../buffer/type_ring_buffer.hpp"
#include "../thread/thread.hpp"
#include "../resilience/circuit_breaker.hpp"
#include "../resilience/rate_limiter.hpp"
#include "../algorithm/consistent_hash.hpp"
#include "../algorithm/bloom_filter.hpp"
#include "../algorithm/trie.hpp"
#include "../encoding/huffman.hpp"
#include "../algorithm/mvcc.hpp"
#include "../app/app.hpp"
#include "../config/parser_manager.hpp"
#include "../system/process.hpp"
#include "../system/cpu.hpp"
#include "../system/numa.hpp"
#include "../system/memory.hpp"
#include "../algorithm/balancer.hpp"

#include "../encoding/base64.hpp"
#include "../crypto/sha1.hpp"
#include "../crypto/md5.hpp"
#include "../algorithm/murmur_hash3.hpp"
#include "../crypto/salt.hpp"
#include "../crypto/hmac.hpp"
#include "../crypto/pbkdf2.hpp"
}
