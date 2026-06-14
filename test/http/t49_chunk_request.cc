/**
 * @file T49-H2ChunkedRequestBody.cc
 * @brief HTTP/2 chunk-first request body contract
 */

#include <sstream>

#define private public
#include "http2/kernel/http2_stream.h"
#undef private

#include <cassert>
#include <iostream>

using namespace galay::http2;

int main() {
    Http2Request request;
    assert(request.bodySize() == 0);
    assert(request.bodyChunkCount() == 0);
    assert(request.takeBodyChunks().empty());

    request.setBody(std::string("hello"));
    assert(request.bodySize() == 5);
    assert(request.bodyChunkCount() == 1);
    const auto& single_chunks = request.bodyChunks();
    assert(single_chunks.size() == 1);
    assert(single_chunks[0] == "hello");
    assert(request.coalescedBody() == "hello");
    assert(request.takeSingleBodyChunk() == "hello");
    assert(request.bodySize() == 0);
    assert(request.bodyChunkCount() == 0);

    request.setBody(std::string("world"));
    const auto& recycled_single_chunks = request.bodyChunks();
    assert(recycled_single_chunks.size() == 1);
    assert(recycled_single_chunks[0] == "world");
    assert(request.takeSingleBodyChunk() == "world");

    request.setBody(std::string("hello"));
    auto taken = request.takeBodyChunks();
    assert(taken.size() == 1);
    assert(taken[0] == "hello");
    assert(request.bodySize() == 0);
    assert(request.bodyChunkCount() == 0);

    auto stream = Http2Stream::create(1);
    stream->appendRequestData(std::string("ab"));
    stream->appendRequestData(std::string("cd"));
    assert(stream->request().bodySize() == 4);
    assert(stream->request().bodyChunkCount() == 2);
    const auto& multi_chunks = stream->request().bodyChunks();
    assert(multi_chunks.size() == 2);
    assert(multi_chunks[0] == "ab");
    assert(multi_chunks[1] == "cd");
    assert(stream->request().coalescedBody() == "abcd");

    auto joined = stream->request().takeCoalescedBody();
    assert(joined == "abcd");
    assert(stream->request().bodySize() == 0);
    assert(stream->request().bodyChunkCount() == 0);

    std::cout << "T49-H2ChunkedRequestBody PASS\n";
    return 0;
}
