module;

#include "mcp/module/module_prelude.hpp"

export module galay.mcp;

export {
#include "mcp/common/mcp_error.h"
#include "mcp/common/mcp_json.h"
#include "mcp/common/mcp_base.h"
#include "mcp/common/json_parser.h"
#include "mcp/common/schema_builder.h"
#include "mcp/common/protocol_utils.h"

#include "mcp/client/stdio_client.h"
#include "mcp/client/http_client.h"

#include "mcp/server/stdio_server.h"
#include "mcp/server/http_server.h"
}
