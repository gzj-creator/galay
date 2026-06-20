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
#include "../kernel/rpc_service.h"
#include "../kernel/rpc_server.h"
#include "../kernel/streamsvc.h"
#include "../kernel/rpc_client.h"
#include "../kernel/rpc_stream.h"
#include "../kernel/rpc_discovery.h"
}
