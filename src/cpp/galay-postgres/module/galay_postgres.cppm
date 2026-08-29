module;

#include "module_prelude.hpp"

export module galay.postgres;

// Match the ABI of the regular galay-postgres shared library when this
// transitional interface is consumed by GCC modules.
export extern "C++" {
#include "../base/postgres_config.h"
#include "../base/postgres_error.h"
#include "../base/postgres_log.h"
#include "../base/postgres_value.h"
#include "../protoc/postgres_packet.h"
#include "../protoc/postgres_protocol.h"
#include "../protoc/postgres_auth.h"
#include "../protoc/builder.h"
#include "../async/client.h"
#include "../async/conn_pool.h"
#include "../sync/postgres_client.h"
}
