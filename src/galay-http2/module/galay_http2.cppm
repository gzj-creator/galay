module;

#include "galay-http2/module/module_prelude.hpp"

export module galay.http2;

export {
#include "galay-http2/protoc/http2_base.h"
#include "galay-http2/protoc/http2_error.h"
#include "galay-http2/protoc/http2_frame.h"
#include "galay-http2/builder/http2_frame_builder.h"
#include "galay-http2/protoc/http2_hpack.h"
#include "galay-http2/utils/h2_helper.h"

#include "galay-http2/client/h2c_client.h"
#include "galay-http2/kernel/http2_conn.h"
#include "galay-http2/server/http2_server.h"
#include "galay-http2/kernel/http2_stream.h"
#include "galay-http2/kernel/stream_mgr.h"

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "galay-http2/client/h2_client.h"
#endif
}
