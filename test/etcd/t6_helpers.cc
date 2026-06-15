#include "galay-etcd/base/etcd_internal.h"

#include <iostream>
#include <string>
#include <vector>

using galay::etcd::PipelineOp;
using galay::etcd::PipelineOpType;
using galay::etcd::EtcdWatchEventType;
using galay::etcd::internal::buildDeleteRequestBody;
using galay::etcd::internal::buildGetRequestBody;
using galay::etcd::internal::buildLeaseGrantRequestBody;
using galay::etcd::internal::buildLeaseKeepAliveRequestBody;
using galay::etcd::internal::buildPutRequestBody;
using galay::etcd::internal::buildTxnBody;
using galay::etcd::internal::buildWatchRequestBody;
using galay::etcd::internal::encodeBase64;
using galay::etcd::internal::parseDeleteResponseDeletedCount;
using galay::etcd::internal::parseEtcdSuccessObject;
using galay::etcd::internal::parseGetResponseKvs;
using galay::etcd::internal::parseLeaseGrantResponseId;
using galay::etcd::internal::parseLeaseKeepAliveResponseId;
using galay::etcd::internal::parsePipelineTxnResponse;
using galay::etcd::internal::parsePipelineResponses;
using galay::etcd::internal::parsePutResponse;
using galay::etcd::internal::parseWatchResponse;

namespace
{

int fail(const std::string& message)
{
    std::cerr << "[FAIL] " << message << '\n';
    return 1;
}

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

} // namespace

int main()
{
    const std::string key = "/internal/helpers/key";
    const std::string value = "hello";

    auto put_body = buildPutRequestBody(key, value, 123);
    if (!put_body.has_value()) {
        return fail("buildPutRequestBody failed: " + put_body.error().message());
    }
    if (!contains(*put_body, "\"key\":\"" + encodeBase64(key) + "\"") ||
        !contains(*put_body, "\"value\":\"" + encodeBase64(value) + "\"") ||
        !contains(*put_body, "\"lease\":\"123\"")) {
        return fail("buildPutRequestBody missing expected fields");
    }

    auto get_body = buildGetRequestBody(key, true, 5);
    if (!get_body.has_value()) {
        return fail("buildGetRequestBody failed: " + get_body.error().message());
    }
    if (!contains(*get_body, "\"range_end\"") || !contains(*get_body, "\"limit\":5")) {
        return fail("buildGetRequestBody missing prefix/limit fields");
    }

    auto del_body = buildDeleteRequestBody(key, true);
    if (!del_body.has_value()) {
        return fail("buildDeleteRequestBody failed: " + del_body.error().message());
    }
    if (!contains(*del_body, "\"range_end\"")) {
        return fail("buildDeleteRequestBody missing range_end");
    }

    auto lease_grant_body = buildLeaseGrantRequestBody(10);
    if (!lease_grant_body.has_value() || !contains(*lease_grant_body, "\"TTL\":10")) {
        return fail("buildLeaseGrantRequestBody failed");
    }

    auto keepalive_body = buildLeaseKeepAliveRequestBody(321);
    if (!keepalive_body.has_value() || !contains(*keepalive_body, "\"ID\":\"321\"")) {
        return fail("buildLeaseKeepAliveRequestBody failed");
    }

    auto watch_body = buildWatchRequestBody(key);
    if (!watch_body.has_value() || !contains(*watch_body, "\"create_request\"") ||
        !contains(*watch_body, "\"key\":\"" + encodeBase64(key) + "\"")) {
        return fail("buildWatchRequestBody failed");
    }

    std::vector<PipelineOp> ops;
    ops.push_back(PipelineOp::Put(key, value));
    ops.push_back(PipelineOp::Get(key));
    ops.push_back(PipelineOp::Del(key));

    auto txn_body = buildTxnBody(std::span<const PipelineOp>(ops.data(), ops.size()));
    if (!txn_body.has_value()) {
        return fail("buildTxnBody failed: " + txn_body.error().message());
    }
    if (!contains(*txn_body, "request_put") ||
        !contains(*txn_body, "request_range") ||
        !contains(*txn_body, "request_delete_range")) {
        return fail("buildTxnBody missing operation fragments");
    }

    const std::string range_key = "/internal/helpers/range";
    const std::string range_value = "range-value";
    const std::string response = std::string("{\"succeeded\":true,\"responses\":[")
        + "{\"response_put\":{}},"
        + "{\"response_range\":{\"kvs\":[{\"key\":\"" + encodeBase64(range_key)
        + "\",\"value\":\"" + encodeBase64(range_value) + "\"}]}}"
        + ",{\"response_delete_range\":{\"deleted\":\"2\"}}]}";

    auto root = parseEtcdSuccessObject(response, "parse helper response");
    if (!root.has_value()) {
        return fail("parseEtcdSuccessObject failed: " + root.error().message());
    }

    auto parsed = parsePipelineResponses(root.value(), std::span<const PipelineOp>(ops.data(), ops.size()));
    if (!parsed.has_value()) {
        return fail("parsePipelineResponses failed: " + parsed.error().message());
    }

    if (parsed->size() != 3) {
        return fail("parsePipelineResponses size mismatch");
    }
    if ((*parsed)[0].type != PipelineOpType::Put || !(*parsed)[0].ok) {
        return fail("put pipeline item parse mismatch");
    }
    if ((*parsed)[1].type != PipelineOpType::Get || !(*parsed)[1].ok ||
        (*parsed)[1].kvs.empty() || (*parsed)[1].kvs.front().value != range_value) {
        return fail("get pipeline item parse mismatch");
    }
    if ((*parsed)[2].type != PipelineOpType::Delete || !(*parsed)[2].ok ||
        (*parsed)[2].deleted_count != 2) {
        return fail("delete pipeline item parse mismatch");
    }

    auto put_ok = parsePutResponse("{}");
    if (!put_ok.has_value()) {
        return fail("parsePutResponse should accept empty success object");
    }

    auto get_kvs = parseGetResponseKvs(
        std::string("{\"kvs\":[{\"key\":\"") + encodeBase64("gk")
            + "\",\"value\":\"" + encodeBase64("gv") + "\"}]}");
    if (!get_kvs.has_value() || get_kvs->size() != 1 || get_kvs->front().key != "gk" || get_kvs->front().value != "gv") {
        return fail("parseGetResponseKvs mismatch");
    }

    auto deleted = parseDeleteResponseDeletedCount("{\"deleted\":\"7\"}");
    if (!deleted.has_value() || deleted.value() != 7) {
        return fail("parseDeleteResponseDeletedCount mismatch");
    }

    auto lease_id = parseLeaseGrantResponseId("{\"ID\":\"123\"}");
    if (!lease_id.has_value() || lease_id.value() != 123) {
        return fail("parseLeaseGrantResponseId mismatch");
    }

    auto keepalive_id = parseLeaseKeepAliveResponseId("{\"ID\":\"321\"}", 321);
    if (!keepalive_id.has_value() || keepalive_id.value() != 321) {
        return fail("parseLeaseKeepAliveResponseId mismatch");
    }

    auto keepalive_mismatch = parseLeaseKeepAliveResponseId("{\"ID\":\"322\"}", 321);
    if (keepalive_mismatch.has_value()) {
        return fail("parseLeaseKeepAliveResponseId should fail on id mismatch");
    }

    std::vector<PipelineOpType> op_types{PipelineOpType::Put, PipelineOpType::Get, PipelineOpType::Delete};
    auto txn_parsed = parsePipelineTxnResponse(
        response,
        std::span<const PipelineOpType>(op_types.data(), op_types.size()));
    if (!txn_parsed.has_value() || txn_parsed->size() != 3) {
        return fail("parsePipelineTxnResponse mismatch");
    }

    const std::string watch_value = "watch-value";
    auto watch_response = parseWatchResponse(
        std::string("{\"result\":{\"watch_id\":\"7\",\"created\":true,\"events\":[{\"type\":\"PUT\",\"kv\":{\"key\":\"")
        + encodeBase64(key) + "\",\"value\":\"" + encodeBase64(watch_value) + "\"}}]}}");
    if (!watch_response.has_value()) {
        return fail("parseWatchResponse failed: " + watch_response.error().message());
    }
    if (watch_response->watch_id != 7 || !watch_response->created) {
        return fail("parseWatchResponse metadata mismatch");
    }
    if (watch_response->events.size() != 1 ||
        watch_response->events.front().type != EtcdWatchEventType::Put ||
        watch_response->events.front().kv.key != key ||
        watch_response->events.front().kv.value != watch_value) {
        return fail("parseWatchResponse event mismatch");
    }

    std::cout << "ETCD INTERNAL HELPERS TEST PASSED\n";
    return 0;
}
