module;

#include "galay-rpc/module/module_prelude.hpp"

export module galay.rpc;

export {
#include "galay-rpc/common/rpc_log.h"

#include "galay-rpc/protoc/rpc_base.h"
#include "galay-rpc/protoc/rpc_error.h"
#include "galay-rpc/protoc/rpc_message.h"
#include "galay-rpc/protoc/rpc_codec.h"

#include "galay-rpc/kernel/rpc_conn.h"
#include "galay-rpc/kernel/rpc_service.h"
#include "galay-rpc/kernel/rpc_server.h"
#include "galay-rpc/kernel/streamsvc.h"
#include "galay-rpc/kernel/rpc_client.h"
#include "galay-rpc/kernel/rpc_stream.h"
#include "galay-rpc/kernel/rpc_discovery.h"
}
