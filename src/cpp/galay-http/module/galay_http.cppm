module;

#include "module_prelude.hpp"

export module galay.http;

// Keep the exported declarations ABI-compatible with the regular shared
// library.  Without an explicit C++ linkage specification GCC gives names
// included from a named module module linkage (for example `@galay.http`),
// which cannot be resolved by libgalay-http's ordinary C++ symbols.
export extern "C++" {
#include "../protoc/http_base.h"
#include "../protoc/http_body.h"
#include "../protoc/http_chunk.h"
#include "../protoc/http_error.h"
#include "../protoc/http_header.h"
#include "../protoc/http_request.h"
#include "../protoc/http_response.h"

#include "../client/http_client.h"
#include "../kernel/http_conn.h"
#include "../kernel/http_reader.h"
#include "../server/http_router.h"
#include "../plugin/common/defn.h"
#include "../plugin/common/conn_info_storage.hpp"
#include "../plugin/blacklist/blacklist.hpp"
#include "../server/http_server.h"
#include "../kernel/http_session.h"
#include "../kernel/http_writer.h"

#include "../builder/http_builder.h"
#include "../utils/http_helper.h"
}
