module;

#include "module_prelude.hpp"

export module galay.mcp;

export {
#include "../common/mcp_error.h"
#include "../common/mcp_json.h"
#include "../common/mcp_base.h"
#include "../common/json_parser.h"
#include "../common/schema_builder.h"
#include "../common/protocol_utils.h"

#include "../client/client.h"

#include "../server/stdio_server.h"
#include "../server/http_server.h"
}
