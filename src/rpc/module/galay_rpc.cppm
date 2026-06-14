module;

#include "rpc/module/module_prelude.hpp"

export module galay.rpc;

export {
#include "rpc/common/rpc_log.h"

#include "rpc/protoc/rpc_base.h"
#include "rpc/protoc/rpc_error.h"
#include "rpc/protoc/rpc_message.h"
#include "rpc/protoc/rpc_codec.h"

#include "rpc/kernel/rpc_conn.h"
#include "rpc/kernel/rpc_service.h"
#include "rpc/kernel/rpc_server.h"
#include "rpc/kernel/streamsvc.h"
#include "rpc/kernel/rpc_client.h"
#include "rpc/kernel/rpc_stream.h"
#include "rpc/kernel/rpc_discovery.h"
}
