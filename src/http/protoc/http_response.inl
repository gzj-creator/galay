#ifndef GALAY_HTTP_RESPONSE_INL
#define GALAY_HTTP_RESPONSE_INL 

#include "http_response.h"

namespace galay::http
{
    template <HttpBodyType T>
    inline T HttpResponse::getBody()
    {
        T body;
        body.fromString(std::move(m_body));
        return body;
    }

    template <HttpBodyType T>
    inline void HttpResponse::setBody(T &&body)
    {
        m_body = body.toString();
        m_header.headerPairs().addHeaderPair("Content-Length", std::to_string(m_body.size()));
        m_header.headerPairs().addHeaderPair("Content-Type", body.contentType());
    }
}

#endif
