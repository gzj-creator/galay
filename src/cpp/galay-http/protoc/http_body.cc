#include "http_body.h"

namespace galay::http {
    PlainBody PlainBody::clone() const
    {
        PlainBody copy;
        copy.m_body = m_body;
        return copy;
    }

    bool PlainBody::from_string(std::string &&str)
    {
        m_body = std::move(str);
        return true;
    }

    std::string PlainBody::to_string()
    {
        return std::move(m_body);
    }

}
