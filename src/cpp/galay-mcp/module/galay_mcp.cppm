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
#include "../v2/common/protocol.h"
#include "../v2/common/http_headers.h"
#include "../v2/server/stdio_server.h"
#include "../v2/server/http_server.h"
#include "../v2/client/client.h"

#include "../v1/client/client.h"

#include "../v1/server/stdio_server.h"
#include "../v1/server/http_server.h"
}
