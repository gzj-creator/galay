#include <galay/cpp/galay-etcd/base/etcd_internal.h>

#include <iostream>
#include <string>
#include <vector>

using galay::etcd::PipelineOp;
using galay::etcd::PipelineOpType;
using galay::etcd::EtcdWatchEventType;
using galay::etcd::internal::build_delete_request_body;
using galay::etcd::internal::build_get_request_body;
using galay::etcd::internal::build_lease_grant_request_body;
using galay::etcd::internal::build_lease_keep_alive_request_body;
using galay::etcd::internal::build_put_request_body;
using galay::etcd::internal::build_txn_body;
using galay::etcd::internal::build_watch_request_body;
using galay::etcd::internal::encode_base64;
using galay::etcd::internal::parse_delete_response_deleted_count;
using galay::etcd::internal::parse_etcd_success_object;
using galay::etcd::internal::parse_get_response_kvs;
using galay::etcd::internal::parse_lease_grant_response_id;
using galay::etcd::internal::parse_lease_keep_alive_response_id;
using galay::etcd::internal::parse_pipeline_txn_response;
using galay::etcd::internal::parse_pipeline_responses;
using galay::etcd::internal::parse_put_response;
using galay::etcd::internal::parse_watch_response;

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

    auto put_body = build_put_request_body(key, value, 123);
    if (!put_body.has_value()) {
        return fail("buildPutRequestBody failed: " + put_body.error().message());
    }
    if (!contains(*put_body, "\"key\":\"" + encode_base64(key) + "\"") ||
        !contains(*put_body, "\"value\":\"" + encode_base64(value) + "\"") ||
        !contains(*put_body, "\"lease\":\"123\"")) {
        return fail("buildPutRequestBody missing expected fields");
    }

    auto get_body = build_get_request_body(key, true, 5);
    if (!get_body.has_value()) {
        return fail("buildGetRequestBody failed: " + get_body.error().message());
    }
    if (!contains(*get_body, "\"range_end\"") || !contains(*get_body, "\"limit\":5")) {
        return fail("buildGetRequestBody missing prefix/limit fields");
    }

    auto del_body = build_delete_request_body(key, true);
    if (!del_body.has_value()) {
        return fail("buildDeleteRequestBody failed: " + del_body.error().message());
    }
    if (!contains(*del_body, "\"range_end\"")) {
        return fail("buildDeleteRequestBody missing range_end");
    }

    auto lease_grant_body = build_lease_grant_request_body(10);
    if (!lease_grant_body.has_value() || !contains(*lease_grant_body, "\"TTL\":10")) {
        return fail("buildLeaseGrantRequestBody failed");
    }

    auto keepalive_body = build_lease_keep_alive_request_body(321);
    if (!keepalive_body.has_value() || !contains(*keepalive_body, "\"ID\":\"321\"")) {
        return fail("buildLeaseKeepAliveRequestBody failed");
    }

    auto watch_body = build_watch_request_body(key);
    if (!watch_body.has_value() || !contains(*watch_body, "\"create_request\"") ||
        !contains(*watch_body, "\"key\":\"" + encode_base64(key) + "\"")) {
        return fail("buildWatchRequestBody failed");
    }

    std::vector<PipelineOp> ops;
    ops.push_back(PipelineOp::put(key, value));
    ops.push_back(PipelineOp::get(key));
    ops.push_back(PipelineOp::del(key));

    auto txn_body = build_txn_body(std::span<const PipelineOp>(ops.data(), ops.size()));
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
        + "{\"response_range\":{\"kvs\":[{\"key\":\"" + encode_base64(range_key)
        + "\",\"value\":\"" + encode_base64(range_value) + "\"}]}}"
        + ",{\"response_delete_range\":{\"deleted\":\"2\"}}]}";

    auto root = parse_etcd_success_object(response, "parse helper response");
    if (!root.has_value()) {
        return fail("parseEtcdSuccessObject failed: " + root.error().message());
    }

    auto parsed = parse_pipeline_responses(root.value(), std::span<const PipelineOp>(ops.data(), ops.size()));
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

    auto put_ok = parse_put_response("{}");
    if (!put_ok.has_value()) {
        return fail("parsePutResponse should accept empty success object");
    }

    auto get_kvs = parse_get_response_kvs(
        std::string("{\"kvs\":[{\"key\":\"") + encode_base64("gk")
            + "\",\"value\":\"" + encode_base64("gv") + "\"}]}");
    if (!get_kvs.has_value() || get_kvs->size() != 1 || get_kvs->front().key != "gk" || get_kvs->front().value != "gv") {
        return fail("parseGetResponseKvs mismatch");
    }

    auto deleted = parse_delete_response_deleted_count("{\"deleted\":\"7\"}");
    if (!deleted.has_value() || deleted.value() != 7) {
        return fail("parseDeleteResponseDeletedCount mismatch");
    }

    auto lease_id = parse_lease_grant_response_id("{\"ID\":\"123\"}");
    if (!lease_id.has_value() || lease_id.value() != 123) {
        return fail("parseLeaseGrantResponseId mismatch");
    }

    auto keepalive_id = parse_lease_keep_alive_response_id("{\"ID\":\"321\"}", 321);
    if (!keepalive_id.has_value() || keepalive_id.value() != 321) {
        return fail("parseLeaseKeepAliveResponseId mismatch");
    }

    auto keepalive_mismatch = parse_lease_keep_alive_response_id("{\"ID\":\"322\"}", 321);
    if (keepalive_mismatch.has_value()) {
        return fail("parseLeaseKeepAliveResponseId should fail on id mismatch");
    }

    std::vector<PipelineOpType> op_types{PipelineOpType::Put, PipelineOpType::Get, PipelineOpType::Delete};
    auto txn_parsed = parse_pipeline_txn_response(
        response,
        std::span<const PipelineOpType>(op_types.data(), op_types.size()));
    if (!txn_parsed.has_value() || txn_parsed->size() != 3) {
        return fail("parsePipelineTxnResponse mismatch");
    }

    const std::string watch_value = "watch-value";
    auto watch_response = parse_watch_response(
        std::string("{\"result\":{\"watch_id\":\"7\",\"created\":true,\"events\":[{\"type\":\"PUT\",\"kv\":{\"key\":\"")
        + encode_base64(key) + "\",\"value\":\"" + encode_base64(watch_value) + "\"}}]}}");
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
