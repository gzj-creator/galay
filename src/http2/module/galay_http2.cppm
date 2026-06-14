module;

#include "http2/module/module_prelude.hpp"

export module galay.http2;

export {
#include "http2/protoc/http2_base.h"
#include "http2/protoc/http2_error.h"
#include "http2/protoc/http2_frame.h"
#include "http2/builder/http2_frame_builder.h"
#include "http2/protoc/http2_hpack.h"
#include "http2/utils/h2_helper.h"

#include "http2/client/h2c_client.h"
#include "http2/kernel/http2_conn.h"
#include "http2/server/http2_server.h"
#include "http2/kernel/http2_stream.h"
#include "http2/kernel/stream_mgr.h"

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "http2/client/h2_client.h"
#endif
}
