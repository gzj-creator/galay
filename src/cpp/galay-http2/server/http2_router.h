#ifndef GALAY_HTTP2_ROUTER_H
#define GALAY_HTTP2_ROUTER_H

#include "../../galay-http/server/http_router.h"
#include "../kernel/http2_stream.h"

namespace galay::http2::server_detail {

kernel::Task<void> execute_http2_route(std::shared_ptr<http::HttpRouter> router,
                                     http2::Http2Stream::ptr stream);

} // namespace galay::http2::server_detail

#endif
