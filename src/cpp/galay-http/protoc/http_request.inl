#ifndef GALAY_HTTP_REQUEST_INL
#define GALAY_HTTP_REQUEST_INL 

#include "http_request.h"

namespace galay::http
{
    template <HttpBodyType T>
    inline T HttpRequest::get_body()
    {
        T body;
        body.from_string(std::move(m_body));
        return body;
    }

    template <HttpBodyType T>
    inline void HttpRequest::set_body(T &&body)
    {
        m_body = body.to_string();
        m_header.header_pairs().add_header_pair("Content-Length", std::to_string(m_body.size()));
        m_header.header_pairs().add_header_pair("Content-Type", body.content_type());
    }
}

#endif
