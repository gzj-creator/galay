#include "result_writer.h"

#include <galay/cpp/galay-rpc/kernel/rpc_call.h>
#include <galay/cpp/galay-rpc/kernel/rpc_metadata.h>

#include <chrono>
#include <string>

using namespace galay::rpc;

namespace {

bool expect(test::TestResultWriter& writer, bool condition, const char* message) {
    writer.writeTestCase(message, condition);
    return condition;
}

}  // namespace

int main() {
    using namespace std::chrono_literals;

    test::TestResultWriter writer("rpc_t10_call_options_results.txt");

    RpcCallOptions defaults;
    if (!expect(writer, !defaults.deadline().has_value(), "default options should not have a deadline")) {
        return 1;
    }
    if (!expect(writer, !defaults.idempotent(), "default options should not be idempotent")) {
        return 1;
    }
    if (!expect(writer, !defaults.maxAttempts().has_value(), "default options should not override max attempts")) {
        return 1;
    }

    auto base = RpcClock::now();
    RpcCallOptions relative;
    relative.timeout(250ms);
    auto relative_deadline = relative.effectiveDeadline(base);
    if (!expect(writer, relative_deadline.has_value(), "relative timeout should produce an effective deadline")) {
        return 1;
    }
    if (!expect(writer, *relative_deadline == base + 250ms, "relative timeout should be relative to the supplied base time")) {
        return 1;
    }

    RpcCallOptions absolute;
    absolute.timeout(250ms);
    absolute.deadline(base + 1s);
    auto absolute_deadline = absolute.effectiveDeadline(base);
    if (!expect(writer, absolute_deadline.has_value(), "absolute deadline should produce an effective deadline")) {
        return 1;
    }
    if (!expect(writer, *absolute_deadline == base + 1s, "absolute deadline should win over relative timeout")) {
        return 1;
    }

    RpcMetadata metadata;
    if (!expect(writer, metadata.insert("traceparent", "abc").has_value(), "valid metadata key should insert")) {
        return 1;
    }
    if (!expect(writer, metadata.insert("bin-value", std::string("\0\1", 2)).has_value(),
                "metadata should accept byte values")) {
        return 1;
    }
    auto value = metadata.get("traceparent");
    if (!expect(writer, value.has_value() && *value == "abc", "metadata lookup should return inserted value")) {
        return 1;
    }
    if (!expect(writer, metadata.remove("traceparent"), "metadata remove should report an existing key")) {
        return 1;
    }
    if (!expect(writer, !metadata.get("traceparent").has_value(), "metadata value should be absent after remove")) {
        return 1;
    }

    if (!expect(writer, !metadata.insert("", "bad").has_value(), "empty metadata key should be rejected")) {
        return 1;
    }
    if (!expect(writer, !metadata.insert("bad key", "bad").has_value(), "metadata key with spaces should be rejected")) {
        return 1;
    }
    if (!expect(writer, !metadata.insert("bad:key", "bad").has_value(), "metadata key with separators should be rejected")) {
        return 1;
    }
    if (!expect(writer, !metadata.insert(std::string(kRpcMetadataMaxKeySize + 1, 'a'), "bad").has_value(),
                "oversized metadata key should be rejected")) {
        return 1;
    }
    if (!expect(writer, !metadata.insert("too-large", std::string(kRpcMetadataMaxValueSize + 1, 'x')).has_value(),
                "oversized metadata value should be rejected")) {
        return 1;
    }

    RpcCallOptions configured;
    configured.idempotent(true).maxAttempts(3);
    configured.metadata().insert("x-request-id", "42");
    if (!expect(writer, configured.idempotent(), "idempotent flag should be configurable")) {
        return 1;
    }
    if (!expect(writer, configured.maxAttempts().has_value() && *configured.maxAttempts() == 3,
                "max attempts override should be configurable")) {
        return 1;
    }
    if (!expect(writer, configured.metadata().get("x-request-id").value_or("") == "42",
                "call options should own per-call metadata")) {
        return 1;
    }

    writer.writeSummary();
    return 0;
}
