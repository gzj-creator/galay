/**
 * @file T30-H2ErrorModel.cc
 * @brief HTTP/2 runtime error model contract test
 */

#include "http2/protoc/http2_error.h"
#include <cassert>
#include <iostream>

using namespace galay::http2;

int main() {
    Http2RuntimeError err = Http2RuntimeError::ProtocolViolation;
    std::string err_text = http2RuntimeErrorToString(err);
    assert(!err_text.empty());

    assert(http2IsConnectionFatal(Http2RuntimeError::ProtocolViolation));
    assert(http2IsConnectionFatal(Http2RuntimeError::FlowControlViolation));
    assert(!http2IsConnectionFatal(Http2RuntimeError::StreamReset));
    assert(!http2IsConnectionFatal(Http2RuntimeError::StreamClosed));
    assert(!http2IsConnectionFatal(Http2RuntimeError::Timeout));
    assert(!http2IsConnectionFatal(Http2RuntimeError::PeerClosed));

    std::cout << "T30-H2ErrorModel PASS\n";
    return 0;
}
