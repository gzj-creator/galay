#ifndef GALAY_API_HTTP2_ADAPTER_H
#define GALAY_API_HTTP2_ADAPTER_H

#ifdef GALAY_API_HTTP2_FEATURE_ENABLED
#include "api_contract.h"
#include <galay/cpp/galay-http2/kernel/http2_stream.h>

namespace galay::api::server_detail {

kernel::Task<void> execute_http2_route(std::shared_ptr<http::HttpRouter> router,
                                     http2::Http2Stream::ptr stream);

} // namespace galay::api::server_detail
#endif

#endif
