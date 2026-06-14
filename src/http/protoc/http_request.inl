#ifndef GALAY_HTTP_REQUEST_INL
#define GALAY_HTTP_REQUEST_INL 

#include "http_request.h"

namespace galay::http
{
    template <HttpBodyType T>
    inline T HttpRequest::getBody()
    {
        T body;
        body.fromString(std::move(m_body));
        return body;
    }

    template <HttpBodyType T>
    inline void HttpRequest::setBody(T &&body)
    {
        m_body = body.toString();
        m_header.headerPairs().addHeaderPair("Content-Length", std::to_string(m_body.size()));
        m_header.headerPairs().addHeaderPair("Content-Type", body.contentType());
    }
}

#endif
