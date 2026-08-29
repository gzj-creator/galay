module;

#include "module_prelude.hpp"

export module galay.redis;

// Match the ABI of the regular galay-redis shared library when this
// transitional interface is consumed by GCC modules.
export extern "C++" {
#include "../base/redis_base.h"
#include "../base/redis_config.h"
#include "../base/redis_error.h"
#include "../base/redis_value.h"
#include "../protoc/redis_protocol.h"
#include "../protoc/connection.h"
#include "../async/client.h"
#include "../async/conn_pool.h"
#include "../async/topology_client.h"
}
