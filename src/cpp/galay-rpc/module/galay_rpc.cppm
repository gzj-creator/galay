module;

#include "module_prelude.hpp"

export module galay.rpc;

export {
#include "../common/rpc_log.h"

#include "../protoc/rpc_base.h"
#include "../protoc/rpc_error.h"
#include "../protoc/rpc_message.h"
#include "../protoc/rpc_codec.h"

#include "../kernel/rpc_conn.h"
#include "../kernel/rpc_metadata.h"
#include "../kernel/rpc_call.h"
#include "../kernel/rpc_policy.h"
#include "../kernel/rpc_config.h"
#include "../config/rpc_config_loader.h"
#include "../kernel/rpc_metrics.h"
#include "../kernel/rpc_tracing.h"
#include "../kernel/rpc_endpoint.h"
#include "../kernel/rpc_endpoint_cache.h"
#include "../kernel/rpc_channel.h"
#include "../kernel/rpc_connection_pool.h"
#include "../kernel/rpc_reconnect.h"
#include "../kernel/rpc_managed_client.h"
#include "../kernel/rpc_interceptor.h"
#include "../kernel/rpc_tls.h"
#include "../kernel/rpc_service.h"
#include "../kernel/rpc_server.h"
#include "../kernel/streamsvc.h"
#include "../kernel/rpc_client.h"
#include "../kernel/rpc_stream.h"
#include "../kernel/rpc_discovery.h"
#include "../discovery/etcd_service_registry.h"
}
