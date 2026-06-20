module;

#include "module_prelude.hpp"

export module galay.http2;

export {
#include "../protoc/http2_base.h"
#include "../protoc/http2_error.h"
#include "../protoc/http2_frame.h"
#include "../builder/http2_frame_builder.h"
#include "../protoc/http2_hpack.h"
#include "../utils/h2_helper.h"

#include "../client/h2c_client.h"
#include "../kernel/http2_conn.h"
#include "../server/http2_server.h"
#include "../kernel/http2_stream.h"
#include "../kernel/stream_manager.h"

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "../client/h2_client.h"
#endif
}
