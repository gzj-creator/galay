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
#include "../v1/protocol.h"
#include "../v1/client.h"
#include "../v1/stdio_server.h"
#include "../v1/http_server.h"
#include "../v2/protocol.h"
#include "../v2/http_headers.h"
#include "../v2/stdio_server.h"
#include "../v2/http_server.h"
#include "../v2/client.h"

#include "../client/client.h"

#include "../server/stdio_server.h"
#include "../server/http_server.h"
}
