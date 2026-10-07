/**
 * @file t154_mpsc_unbounded_source.cc
 * @brief 锁定 MPSC unbounded channel 的独立数据面与单消费者热路径。
 */

#include "result_writer.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::string read_all(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string extract_function(const std::string& content, const std::string& marker)
{
    const size_t begin = content.find(marker);
    if (begin == std::string::npos) {
        return {};
    }
    const size_t opening = content.find('{', begin + marker.size());
    if (opening == std::string::npos) {
        return {};
    }

    size_t depth = 0;
    for (size_t index = opening; index < content.size(); ++index) {
        if (content[index] == '{') {
            ++depth;
        } else if (content[index] == '}') {
            if (--depth == 0) {
                return content.substr(begin, index + 1 - begin);
            }
        }
    }
    return {};
}

void require_contains(std::vector<std::string>& failures,
                     const std::string& content,
                     const std::string& needle,
                     const std::string& message)
{
    if (content.find(needle) == std::string::npos) {
        failures.push_back(message);
    }
}

void require_not_contains(std::vector<std::string>& failures,
                        const std::string& content,
                        const std::string& needle,
                        const std::string& message)
{
    if (content.find(needle) != std::string::npos) {
        failures.push_back(message);
    }
}

void require_ordered(std::vector<std::string>& failures,
                    const std::string& content,
                    const std::string& first,
                    const std::string& second,
                    const std::string& message)
{
    const size_t firstPosition = content.rfind(first);
    const size_t secondPosition = content.rfind(second);
    if (firstPosition == std::string::npos ||
        secondPosition == std::string::npos ||
        firstPosition >= secondPosition) {
        failures.push_back(message);
    }
}

} // namespace

int main()
{
    galay::test::TestResultWriter writer("t154_mpsc_unbounded_source");
    writer.add_test();

    const std::filesystem::path header = std::filesystem::path(GALAY_SOURCE_ROOT) /
        "galay-kernel" / "concurrency" / "mpsc" / "unbounded_channel.h";
    const std::string content = read_all(header);
    std::vector<std::string> failures;
    if (content.empty()) {
        failures.push_back("failed to read " + header.string());
    } else {
        require_not_contains(failures,
                           content,
                           "concurrentqueue/moodycamel",
                           "MPSC unbounded must not include moodycamel");
        require_not_contains(failures,
                           content,
                           "moodycamel::",
                           "MPSC unbounded must not use an MPMC data plane");
        require_not_contains(failures,
                           content,
                           "std::default_initializable",
                           "MPSC storage must not require default construction");
        require_contains(failures,
                        content,
                        "thread_local",
                        "default send must cache a producer-local stream");
        require_contains(failures,
                        content,
                        "ProducerStream",
                        "MPSC must own per-producer SPSC streams");
        require_not_contains(failures,
                           content,
                           "m_streamCount",
                           "append-only stream traversal must not maintain a shared count");
        require_contains(failures,
                        content,
                        "m_readyStack",
                        "consumer must discover only active producer streams");
        require_contains(failures,
                        content,
                        "active.compare_exchange_strong",
                        "active stream discovery must use a single membership transition");
        require_contains(failures,
                        content,
                        "recycledBlocks",
                        "retired producer blocks must be recycled across the SPSC boundary");
        require_contains(failures,
                        content,
                        "construction_address",
                        "raw storage construction must not call std::launder before lifetime begins");
        require_not_contains(
            failures,
            content,
            "std::array<std::atomic<uint8_t>, kBlockCapacity> ready{}",
            "blocks must not carry per-slot ready atomics");
        require_contains(failures,
                        content,
                        "uint64_t observedPublished = 0;",
                        "consumer must cache each stream's cumulative published tail");
        const std::string slot = extract_function(content, "struct Slot");
        if (slot.empty()) {
            failures.push_back("failed to locate MPSC slot storage");
        } else {
            require_not_contains(failures,
                               slot,
                               "std::atomic",
                               "tail publication must not increase the slot stride");
        }
        require_contains(
            failures,
            content,
            "(kBlockTargetBytes - ::galay::utils::kCacheLineSize)",
            "block capacity must remain derived from the original slot storage");
        require_contains(failures,
                        content,
                        "kWaking",
                        "producer wake must keep the waiter slot non-rearmable until consumed");
        require_contains(failures,
                        content,
                        "kPublished",
                        "producer gate must distinguish published data from active construction");

        const std::string tokenValidity = extract_function(
            content,
            "bool valid_for(const UnboundedChannel* channel) const noexcept");
        if (tokenValidity.empty()) {
            failures.push_back("failed to locate MPSC producer token validity check");
        } else {
            require_contains(
                failures,
                tokenValidity,
                "lifetime->state.load(std::memory_order_acquire)",
                "producer token validity must inspect its retained lifetime state");
            require_contains(
                failures,
                tokenValidity,
                "ProducerLifetimeState::kOwned",
                "producer token validity must reject detached or relinquished streams");
            require_ordered(
                failures,
                tokenValidity,
                "lifetime->state.load(std::memory_order_acquire)",
                "m_channel == channel",
                "producer token validity must reject detached lifetime before comparing an expired channel pointer");
        }

        const std::string liveTokenValidity = extract_function(
            content,
            "bool valid_for_live_channel(\n"
            "            const UnboundedChannel* channel) const noexcept");
        if (liveTokenValidity.empty()) {
            failures.push_back("failed to locate MPSC live-channel token check");
        } else {
            require_not_contains(
                failures,
                liveTokenValidity,
                ".load(",
                "live-channel token send hot path must not load lifetime state");
            require_not_contains(
                failures,
                liveTokenValidity,
                "ProducerLifetimeState",
                "live-channel token send hot path must not inspect cold lifetime state");
        }

        const std::string tokenSend = extract_function(
            content, "bool send(ProducerToken& token, T&& value) noexcept");
        if (tokenSend.empty()) {
            failures.push_back("failed to locate MPSC token single send");
        } else {
            require_contains(
                failures,
                tokenSend,
                "token.valid_for_live_channel(this)",
                "token single send must retain the live-channel hot-path check");
            require_not_contains(
                failures,
                tokenSend,
                "token.valid_for(this)",
                "token single send must not restore the lifetime acquire");
        }

        const std::string tryRecv =
            extract_function(content, "std::optional<T> try_recv()");
        if (tryRecv.empty()) {
            failures.push_back("failed to locate MPSC try_recv");
        } else {
            require_not_contains(failures,
                               tryRecv,
                               "fetch_add",
                               "single consumer try_recv must not execute an RMW");
            require_not_contains(failures,
                               tryRecv,
                               "compare_exchange",
                               "single consumer try_recv must not execute CAS");
            require_not_contains(failures,
                               tryRecv,
                               "try_dequeue",
                               "single consumer try_recv must not call MPMC dequeue");
        }

        const std::string beginSend =
            extract_function(content, "bool begin_send(ProducerStream& stream) noexcept");
        if (beginSend.empty()) {
            failures.push_back("failed to locate MPSC begin_send");
        } else {
            require_contains(failures,
                            beginSend,
                            "stream.control.gate.store(\n"
                            "            ProducerGate::kSending,\n"
                            "            std::memory_order_seq_cst)",
                            "send permit must announce in-flight state with a seq_cst store");
            require_not_contains(failures,
                               beginSend,
                               "test_and_set",
                            "send permit must not use atomic test-and-set");
            require_not_contains(failures,
                               beginSend,
                               ".exchange(",
                               "send permit must not use atomic exchange");
            require_not_contains(failures,
                               beginSend,
                               "compare_exchange",
                               "steady-state send permit must not use CAS");
            require_not_contains(failures,
                               beginSend,
                               "fetch_",
                               "send permit must not use atomic fetch RMW");
            require_contains(failures,
                            beginSend,
                            "m_closeState.load(std::memory_order_seq_cst)",
                            "send permit must share the close cutoff seq_cst order");
            require_contains(failures,
                            beginSend,
                            "stream.control.gate.store(ProducerGate::kOpen,\n"
                            "                                  std::memory_order_release)",
                            "rejected send must clear its producer-owned in-flight flag");
        }

        const std::string finishSend =
            extract_function(content, "void finish_send(ProducerStream& stream) noexcept");
        if (finishSend.empty()) {
            failures.push_back("failed to locate MPSC finish_send");
        } else {
            require_contains(failures,
                            finishSend,
                            "std::memory_order_release",
                            "completed send must release the producer gate");
        }

        const std::string reserveProducerSlots = extract_function(
            content,
            "bool reserve_producer_slots(ProducerStream& stream, size_t count) noexcept");
        if (reserveProducerSlots.empty()) {
            failures.push_back("failed to locate MPSC producer slot reservation");
        } else {
            require_ordered(
                failures,
                reserveProducerSlots,
                "producer.block = first;",
                "previousTail->next.store(first, std::memory_order_release)",
                "a full-block producer cursor must advance before the old link becomes recyclable");
        }

        const std::string takeRecycledBlock = extract_function(
            content,
            "Block* take_recycled_block(ProducerStream& stream) noexcept");
        if (takeRecycledBlock.empty()) {
            failures.push_back("failed to locate MPSC recycled block acquisition");
        } else {
            require_not_contains(failures,
                               takeRecycledBlock,
                               "ready",
                               "recycled blocks must not retain per-slot ready atomics");
            require_contains(
                failures,
                takeRecycledBlock,
                "block->next.store(nullptr, std::memory_order_relaxed)",
                "recycled blocks must clear their old chain link");
        }

        const std::string publishStream = extract_function(
            content,
            "TaskState* publish_stream(ProducerStream& stream,");
        if (publishStream.empty()) {
            failures.push_back("failed to locate MPSC stream publication");
        } else {
            require_not_contains(
                failures,
                publishStream,
                "ready[",
                "stream publication must not touch per-slot ready atomics");
            require_contains(
                failures,
                publishStream,
                "stream.shared.published.store(producer.localPublished,\n"
                "                                      std::memory_order_release)",
                "diagnostic publication counter must remain a release snapshot");
            require_not_contains(
                failures,
                publishStream,
                "published.store(producer.localPublished,\n"
                "                                      std::memory_order_seq_cst)",
                "consumer-shared publication counter must not use seq_cst store");
            require_contains(
                failures,
                publishStream,
                "stream.control.gate.store(\n"
                "            ProducerGate::kPublished,\n"
                "            std::memory_order_seq_cst)",
                "published data must join waiter ordering through the producer gate");
            require_ordered(
                failures,
                publishStream,
                "activate_ready_stream(stream)",
                "published.store(producer.localPublished",
                "ready stream membership must precede cumulative tail publication");
            require_ordered(
                failures,
                publishStream,
                "published.store(producer.localPublished",
                "ProducerGate::kPublished",
                "data publication must precede the published gate announcement");
            require_ordered(
                failures,
                publishStream,
                "ProducerGate::kPublished",
                "detach_published_waiter()",
                "published gate announcement must precede waiter arbitration");
        }

        const std::string singleSend = extract_function(
            content,
            "bool send_to_stream(ProducerStream& stream, T&& value) noexcept");
        if (singleSend.empty()) {
            failures.push_back("failed to locate MPSC single send");
        } else {
            require_contains(failures,
                            singleSend,
                            "publish_stream(stream, 1);",
                            "empty single-send wake path must use a nullable raw waiter state");
            require_not_contains(failures,
                               singleSend,
                               "TaskRef waiter_task",
                               "empty single-send wake path must not construct TaskRef");
            require_contains(failures,
                            singleSend,
                            "if (waiterState != nullptr)",
                            "empty single-send wake path must bypass the out-of-line waker");
            require_ordered(failures,
                           singleSend,
                           "publish_stream(stream, 1)",
                           "finish_send(stream);",
                           "single send must publish before releasing its producer gate");
            require_ordered(failures,
                           singleSend,
                           "finish_send(stream);",
                           "wake_detached_waiter",
                           "single send must release its producer gate before waking a waiter");
        }

        const std::string copyBatchSend = extract_function(
            content,
            "ProducerStream& stream, const std::vector<T>& values)");
        if (copyBatchSend.empty()) {
            failures.push_back("failed to locate MPSC copy batch send");
        } else {
            require_not_contains(failures,
                               copyBatchSend,
                               "TaskRef waiter_task",
                               "empty copy-batch wake path must not construct TaskRef");
            require_contains(failures,
                            copyBatchSend,
                            "if (waiterState != nullptr)",
                            "empty copy-batch wake path must bypass the out-of-line waker");
            require_ordered(failures,
                           copyBatchSend,
                           "publish_stream(stream, values.size())",
                           "finish_send(stream);",
                           "copy batch send must publish before releasing its producer gate");
            require_ordered(failures,
                           copyBatchSend,
                           "finish_send(stream);",
                           "wake_detached_waiter",
                           "copy batch send must release its producer gate before waking a waiter");
        }

        const std::string moveBatchSend = extract_function(
            content,
            "ProducerStream& stream, std::vector<T>&& values) noexcept");
        if (moveBatchSend.empty()) {
            failures.push_back("failed to locate MPSC move batch send");
        } else {
            require_not_contains(failures,
                               moveBatchSend,
                               "TaskRef waiter_task",
                               "empty move-batch wake path must not construct TaskRef");
            require_contains(failures,
                            moveBatchSend,
                            "if (waiterState != nullptr)",
                            "empty move-batch wake path must bypass the out-of-line waker");
            size_t finishCount = 0;
            size_t position = 0;
            while ((position = moveBatchSend.find("finish_send(stream);", position)) !=
                   std::string::npos) {
                ++finishCount;
                position += sizeof("finish_send(stream);") - 1;
            }
            if (finishCount != 2) {
                failures.push_back(
                    "move batch send must release the producer gate exactly once per exit path");
            }
            require_ordered(failures,
                           moveBatchSend,
                           "publish_stream(stream, values.size())",
                           "finish_send(stream);",
                           "move batch send must publish before releasing its producer gate");
            require_ordered(failures,
                           moveBatchSend,
                           "finish_send(stream);",
                           "wake_detached_waiter",
                           "move batch send must release its producer gate before waking a waiter");
        }

        const std::string popStream = extract_function(
            content,
            "std::optional<T> try_pop_stream(ProducerStream& stream) noexcept");
        if (popStream.empty()) {
            failures.push_back("failed to locate MPSC stream consumer hot path");
        } else {
            require_contains(
                failures,
                popStream,
                "stream.shared.published.load(std::memory_order_acquire)",
                "consumer must acquire a fresh cumulative producer tail");
            require_not_contains(
                failures,
                popStream,
                "ready[",
                "consumer hot path must not access per-slot ready atomics");
            require_ordered(
                failures,
                popStream,
                "consumer.localConsumed == consumer.observedPublished",
                "if (consumer.index == kBlockCapacity)",
                "consumer must prove data availability before advancing blocks");
        }

        const std::string close = extract_function(content, "bool close() noexcept");
        if (close.empty()) {
            failures.push_back("failed to locate MPSC close");
        } else {
            require_contains(failures,
                            close,
                            "stream->control.gate.load(std::memory_order_seq_cst)",
                            "close must pair its cutoff with the producer seq_cst announcement");
            require_contains(failures,
                            close,
                            "std::memory_order_seq_cst) !=\n"
                            "                   ProducerGate::kOpen",
                            "close must wait for constructing and published producer states");
            require_not_contains(failures,
                               close,
                               "stream->control.gate.store",
                               "close must not write the producer-owned in-flight flag");
            require_contains(failures,
                            close,
                            "m_closeState.store(CloseState::kClosed,\n"
                            "                           std::memory_order_seq_cst)",
                            "terminal close publication must share the waiter seq_cst order");
            require_ordered(failures,
                           close,
                           "detach_published_waiter();",
                           "wake_detached_waiter",
                           "close must detach all channel-owned waiter state before waking");
        }

        const std::string singleAwaitSuspend = extract_function(
            content,
            "bool UnboundedRecvAwaitable<T>::await_suspend(");
        if (singleAwaitSuspend.empty()) {
            failures.push_back("failed to locate MPSC single await_suspend");
        } else {
            const size_t registration =
                singleAwaitSuspend.find("begin_waiter_registration()");
            const size_t publication = singleAwaitSuspend.find("publish_waiter(");
            if (registration == std::string::npos ||
                publication == std::string::npos || registration >= publication) {
                failures.push_back(
                    "single await_suspend must register before publishing waiter");
            } else {
                const std::string armingPath = singleAwaitSuspend.substr(
                    registration, publication - registration);
                require_not_contains(
                    failures,
                    armingPath,
                    "try_receive_now()",
                    "single waiter final check must not rely on a receive probe");
                require_contains(
                    failures,
                    armingPath,
                    "has_published_value_for_waiter()",
                    "single waiter final check must scan producer gates");
            }
        }

        const std::string closedAndDrained =
            extract_function(content, "bool is_closed_and_drained() const noexcept");
        if (closedAndDrained.empty()) {
            failures.push_back("failed to locate MPSC is_closed_and_drained");
        } else {
            require_contains(failures,
                            closedAndDrained,
                            "m_closeState.load(std::memory_order_seq_cst)",
                            "waiter close recheck must share the waiter seq_cst order");
        }

        const std::string batchToAwaitSuspend = extract_function(
            content,
            "bool UnboundedRecvBatchToAwaitable<T>::await_suspend(");
        if (batchToAwaitSuspend.empty()) {
            failures.push_back("failed to locate MPSC batch-to await_suspend");
        } else {
            const size_t registration =
                batchToAwaitSuspend.find("begin_waiter_registration()");
            const size_t publication = batchToAwaitSuspend.find("publish_waiter(");
            if (registration == std::string::npos ||
                publication == std::string::npos || registration >= publication) {
                failures.push_back(
                    "batch-to await_suspend must register before publishing waiter");
            } else {
                const std::string armingPath = batchToAwaitSuspend.substr(
                    registration, publication - registration);
                require_not_contains(
                    failures,
                    armingPath,
                    "try_receive_now()",
                    "batch-to waiter final check must not rely on a drain probe");
                require_contains(
                    failures,
                    armingPath,
                    "has_published_value_for_waiter()",
                    "batch-to waiter final check must scan producer gates");
            }
        }

        const std::string batchAwaitSuspend = extract_function(
            content,
            "bool UnboundedRecvBatchAwaitable<T>::await_suspend(");
        if (batchAwaitSuspend.empty()) {
            failures.push_back("failed to locate MPSC vector batch await_suspend");
        } else {
            const size_t registration =
                batchAwaitSuspend.find("begin_waiter_registration()");
            const size_t publication = batchAwaitSuspend.find("publish_waiter(");
            if (registration == std::string::npos ||
                publication == std::string::npos || registration >= publication) {
                failures.push_back(
                    "vector batch await_suspend must register before publishing waiter");
            } else {
                const std::string armingPath = batchAwaitSuspend.substr(
                    registration, publication - registration);
                require_not_contains(
                    failures,
                    armingPath,
                    "try_receive_now()",
                    "vector batch waiter arming path must not execute allocating receive");
                require_contains(
                    failures,
                    armingPath,
                    "has_published_value_for_waiter()",
                    "vector batch waiter arming path must use a non-allocating readiness check");
            }
        }

        const std::string pushReady =
            extract_function(content, "void push_ready_stream(ProducerStream& stream) noexcept");
        if (pushReady.empty()) {
            failures.push_back("failed to locate MPSC ready stream publication");
        } else {
            require_contains(
                failures,
                pushReady,
                "std::memory_order_seq_cst",
                "first ready stream publication must join waiter discovery SC order");
        }

        const std::string appendReady =
            extract_function(content, "void append_ready_streams() noexcept");
        if (appendReady.empty()) {
            failures.push_back("failed to locate MPSC ready stream discovery");
        } else {
            require_contains(
                failures,
                appendReady,
                "m_readyStack.load(std::memory_order_seq_cst)",
                "ready stream probe must join waiter discovery SC order");
            require_contains(
                failures,
                appendReady,
                "m_readyStack.exchange(nullptr, std::memory_order_seq_cst)",
                "ready stream detach must join waiter discovery SC order");
        }

        const std::string waiterReadiness = extract_function(
            content, "bool has_published_value_for_waiter() noexcept");
        if (waiterReadiness.empty()) {
            failures.push_back("failed to locate MPSC non-allocating waiter readiness check");
        } else {
            require_contains(
                failures,
                waiterReadiness,
                "append_ready_streams();",
                "waiter readiness must discover first-active streams before probing data");
            require_contains(
                failures,
                waiterReadiness,
                "ProducerStream* stream = m_readyHead;",
                "waiter readiness must retain the ready-stack publication path");
            require_contains(
                failures,
                waiterReadiness,
                "m_streamHead.load(std::memory_order_acquire)",
                "waiter readiness must scan every registered producer stream");
            require_contains(
                failures,
                waiterReadiness,
                "control.gate.load(std::memory_order_seq_cst)",
                "waiter readiness must join producer notification ordering through the gate");
            require_contains(
                failures,
                waiterReadiness,
                "ProducerGate::kPublished",
                "waiter readiness must treat a published gate as immediately readable");
            require_contains(
                failures,
                waiterReadiness,
                "ProducerGate::kSending",
                "waiter readiness must identify sends that will arbitrate after arming");
            require_contains(
                failures,
                waiterReadiness,
                "if (gate == ProducerGate::kSending) {\n"
                "                if (stream->consumer.localConsumed !=\n"
                "                    stream->shared.published.load(std::memory_order_acquire))",
                "a sending stream must recheck previously published data before arming");
            require_contains(
                failures,
                waiterReadiness,
                "published.load(std::memory_order_acquire)",
                "open producer streams must acquire the released publication counter");
            require_not_contains(
                failures,
                waiterReadiness,
                "published.load(std::memory_order_seq_cst)",
                "waiter readiness must not restore seq_cst traffic on shared publication");
        }
    }

    if (!failures.empty()) {
        for (const std::string& failure : failures) {
            std::cerr << failure << '\n';
        }
        writer.add_failed();
        writer.write_result();
        return 1;
    }

    writer.add_passed();
    writer.write_result();
    std::cout << "t154_mpsc_unbounded_source PASS\n";
    return 0;
}
