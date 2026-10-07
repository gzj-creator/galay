#ifndef GALAY_HTTP_RESPONSE_INL
#define GALAY_HTTP_RESPONSE_INL 

#include "http_response.h"

namespace galay::http
{
    template <HttpBodyType T>
    inline T HttpResponse::get_body()
    {
        T body;
        body.from_string(std::move(m_body));
        return body;
    }

    template <HttpBodyType T>
    inline void HttpResponse::set_body(T &&body)
    {
        m_body = body.to_string();
        m_header.header_pairs().add_header_pair("Content-Length", std::to_string(m_body.size()));
        m_header.header_pairs().add_header_pair("Content-Type", body.content_type());
    }
}

#endif
