/**
 * @file T49-H2ChunkedRequestBody.cc
 * @brief HTTP/2 chunk-first request body contract
 */

#include <sstream>

#define private public
#include <galay/cpp/galay-http2/kernel/http2_stream.h>
#undef private

#include <cassert>
#include <iostream>

using namespace galay::http2;

int main() {
    Http2Request request;
    assert(request.body_size() == 0);
    assert(request.body_chunk_count() == 0);
    assert(request.take_body_chunks().empty());

    request.set_body(std::string("hello"));
    assert(request.body_size() == 5);
    assert(request.body_chunk_count() == 1);
    const auto& single_chunks = request.body_chunks();
    assert(single_chunks.size() == 1);
    assert(single_chunks[0] == "hello");
    assert(request.coalesced_body() == "hello");
    assert(request.take_single_body_chunk() == "hello");
    assert(request.body_size() == 0);
    assert(request.body_chunk_count() == 0);

    request.set_body(std::string("world"));
    const auto& recycled_single_chunks = request.body_chunks();
    assert(recycled_single_chunks.size() == 1);
    assert(recycled_single_chunks[0] == "world");
    assert(request.take_single_body_chunk() == "world");

    request.set_body(std::string("hello"));
    auto taken = request.take_body_chunks();
    assert(taken.size() == 1);
    assert(taken[0] == "hello");
    assert(request.body_size() == 0);
    assert(request.body_chunk_count() == 0);

    auto stream = Http2Stream::create(1);
    stream->append_request_data(std::string("ab"));
    stream->append_request_data(std::string("cd"));
    assert(stream->request().body_size() == 4);
    assert(stream->request().body_chunk_count() == 2);
    const auto& multi_chunks = stream->request().body_chunks();
    assert(multi_chunks.size() == 2);
    assert(multi_chunks[0] == "ab");
    assert(multi_chunks[1] == "cd");
    assert(stream->request().coalesced_body() == "abcd");

    auto joined = stream->request().take_coalesced_body();
    assert(joined == "abcd");
    assert(stream->request().body_size() == 0);
    assert(stream->request().body_chunk_count() == 0);

    std::cout << "T49-H2ChunkedRequestBody PASS\n";
    return 0;
}
