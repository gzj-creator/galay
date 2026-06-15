module;

#include "galay-mcp/module/module_prelude.hpp"

export module galay.mcp;

export {
#include "galay-mcp/common/mcp_error.h"
#include "galay-mcp/common/mcp_json.h"
#include "galay-mcp/common/mcp_base.h"
#include "galay-mcp/common/json_parser.h"
#include "galay-mcp/common/schema_builder.h"
#include "galay-mcp/common/protocol_utils.h"

#include "galay-mcp/client/client.h"

#include "galay-mcp/server/stdio_server.h"
#include "galay-mcp/server/http_server.h"
}
