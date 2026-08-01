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

std::string readAll(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string extractFunction(const std::string& content, const std::string& marker)
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

void requireContains(std::vector<std::string>& failures,
                     const std::string& content,
                     const std::string& needle,
                     const std::string& message)
{
    if (content.find(needle) == std::string::npos) {
        failures.push_back(message);
    }
}

void requireNotContains(std::vector<std::string>& failures,
                        const std::string& content,
                        const std::string& needle,
                        const std::string& message)
{
    if (content.find(needle) != std::string::npos) {
        failures.push_back(message);
    }
}

void requireOrdered(std::vector<std::string>& failures,
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
    writer.addTest();

    const std::filesystem::path header = std::filesystem::path(GALAY_SOURCE_ROOT) /
        "galay-kernel" / "concurrency" / "mpsc" / "unbounded_channel.h";
    const std::string content = readAll(header);
    std::vector<std::string> failures;
    if (content.empty()) {
        failures.push_back("failed to read " + header.string());
    } else {
        requireNotContains(failures,
                           content,
                           "concurrentqueue/moodycamel",
                           "MPSC unbounded must not include moodycamel");
        requireNotContains(failures,
                           content,
                           "moodycamel::",
                           "MPSC unbounded must not use an MPMC data plane");
        requireNotContains(failures,
                           content,
                           "std::default_initializable",
                           "MPSC storage must not require default construction");
        requireContains(failures,
                        content,
                        "thread_local",
                        "default send must cache a producer-local stream");
        requireContains(failures,
                        content,
                        "ProducerStream",
                        "MPSC must own per-producer SPSC streams");
        requireNotContains(failures,
                           content,
                           "m_streamCount",
                           "append-only stream traversal must not maintain a shared count");
        requireContains(failures,
                        content,
                        "m_readyStack",
                        "consumer must discover only active producer streams");
        requireContains(failures,
                        content,
                        "active.compare_exchange_strong",
                        "active stream discovery must use a single membership transition");
        requireContains(failures,
                        content,
                        "recycledBlocks",
                        "retired producer blocks must be recycled across the SPSC boundary");
        requireContains(failures,
                        content,
                        "constructionAddress",
                        "raw storage construction must not call std::launder before lifetime begins");
        requireContains(failures,
                        content,
                        "kWaking",
                        "producer wake must keep the waiter slot non-rearmable until consumed");

        const std::string tokenValidity = extractFunction(
            content,
            "bool validFor(const UnboundedChannel* channel) const noexcept");
        if (tokenValidity.empty()) {
            failures.push_back("failed to locate MPSC producer token validity check");
        } else {
            requireContains(
                failures,
                tokenValidity,
                "lifetime->state.load(std::memory_order_acquire)",
                "producer token validity must inspect its retained lifetime state");
            requireContains(
                failures,
                tokenValidity,
                "ProducerLifetimeState::kOwned",
                "producer token validity must reject detached or relinquished streams");
            requireOrdered(
                failures,
                tokenValidity,
                "lifetime->state.load(std::memory_order_acquire)",
                "m_channel == channel",
                "producer token validity must reject detached lifetime before comparing an expired channel pointer");
        }

        const std::string tryRecv =
            extractFunction(content, "std::optional<T> tryRecv()");
        if (tryRecv.empty()) {
            failures.push_back("failed to locate MPSC tryRecv");
        } else {
            requireNotContains(failures,
                               tryRecv,
                               "fetch_add",
                               "single consumer tryRecv must not execute an RMW");
            requireNotContains(failures,
                               tryRecv,
                               "compare_exchange",
                               "single consumer tryRecv must not execute CAS");
            requireNotContains(failures,
                               tryRecv,
                               "try_dequeue",
                               "single consumer tryRecv must not call MPMC dequeue");
        }

        const std::string beginSend =
            extractFunction(content, "bool beginSend(ProducerStream& stream) noexcept");
        if (beginSend.empty()) {
            failures.push_back("failed to locate MPSC beginSend");
        } else {
            requireContains(failures,
                            beginSend,
                            "stream.control.gate.store(\n"
                            "            ProducerGate::kSending,\n"
                            "            std::memory_order_seq_cst)",
                            "send permit must announce in-flight state with a seq_cst store");
            requireNotContains(failures,
                               beginSend,
                               "test_and_set",
                            "send permit must not use atomic test-and-set");
            requireNotContains(failures,
                               beginSend,
                               ".exchange(",
                               "send permit must not use atomic exchange");
            requireNotContains(failures,
                               beginSend,
                               "compare_exchange",
                               "steady-state send permit must not use CAS");
            requireNotContains(failures,
                               beginSend,
                               "fetch_",
                               "send permit must not use atomic fetch RMW");
            requireContains(failures,
                            beginSend,
                            "m_closeState.load(std::memory_order_seq_cst)",
                            "send permit must share the close cutoff seq_cst order");
            requireContains(failures,
                            beginSend,
                            "stream.control.gate.store(ProducerGate::kOpen,\n"
                            "                                  std::memory_order_release)",
                            "rejected send must clear its producer-owned in-flight flag");
        }

        const std::string finishSend =
            extractFunction(content, "void finishSend(ProducerStream& stream) noexcept");
        if (finishSend.empty()) {
            failures.push_back("failed to locate MPSC finishSend");
        } else {
            requireContains(failures,
                            finishSend,
                            "std::memory_order_release",
                            "completed send must release the producer gate");
        }

        const std::string singleSend = extractFunction(
            content,
            "bool sendToStream(ProducerStream& stream, T&& value) noexcept");
        if (singleSend.empty()) {
            failures.push_back("failed to locate MPSC single send");
        } else {
            requireContains(failures,
                            singleSend,
                            "TaskState* waiterState = publishStream(stream, 1);",
                            "empty single-send wake path must use a nullable raw waiter state");
            requireNotContains(failures,
                               singleSend,
                               "TaskRef waiterTask",
                               "empty single-send wake path must not construct TaskRef");
            requireContains(failures,
                            singleSend,
                            "if (waiterState != nullptr)",
                            "empty single-send wake path must bypass the out-of-line waker");
            requireOrdered(failures,
                           singleSend,
                           "publishStream(stream, 1)",
                           "finishSend(stream);",
                           "single send must publish before releasing its producer gate");
            requireOrdered(failures,
                           singleSend,
                           "finishSend(stream);",
                           "wakeDetachedWaiter",
                           "single send must release its producer gate before waking a waiter");
        }

        const std::string copyBatchSend = extractFunction(
            content,
            "ProducerStream& stream, const std::vector<T>& values)");
        if (copyBatchSend.empty()) {
            failures.push_back("failed to locate MPSC copy batch send");
        } else {
            requireNotContains(failures,
                               copyBatchSend,
                               "TaskRef waiterTask",
                               "empty copy-batch wake path must not construct TaskRef");
            requireContains(failures,
                            copyBatchSend,
                            "if (waiterState != nullptr)",
                            "empty copy-batch wake path must bypass the out-of-line waker");
            requireOrdered(failures,
                           copyBatchSend,
                           "publishStream(stream, values.size())",
                           "finishSend(stream);",
                           "copy batch send must publish before releasing its producer gate");
            requireOrdered(failures,
                           copyBatchSend,
                           "finishSend(stream);",
                           "wakeDetachedWaiter",
                           "copy batch send must release its producer gate before waking a waiter");
        }

        const std::string moveBatchSend = extractFunction(
            content,
            "ProducerStream& stream, std::vector<T>&& values) noexcept");
        if (moveBatchSend.empty()) {
            failures.push_back("failed to locate MPSC move batch send");
        } else {
            requireNotContains(failures,
                               moveBatchSend,
                               "TaskRef waiterTask",
                               "empty move-batch wake path must not construct TaskRef");
            requireContains(failures,
                            moveBatchSend,
                            "if (waiterState != nullptr)",
                            "empty move-batch wake path must bypass the out-of-line waker");
            size_t finishCount = 0;
            size_t position = 0;
            while ((position = moveBatchSend.find("finishSend(stream);", position)) !=
                   std::string::npos) {
                ++finishCount;
                position += sizeof("finishSend(stream);") - 1;
            }
            if (finishCount != 2) {
                failures.push_back(
                    "move batch send must release the producer gate exactly once per exit path");
            }
            requireOrdered(failures,
                           moveBatchSend,
                           "publishStream(stream, values.size())",
                           "finishSend(stream);",
                           "move batch send must publish before releasing its producer gate");
            requireOrdered(failures,
                           moveBatchSend,
                           "finishSend(stream);",
                           "wakeDetachedWaiter",
                           "move batch send must release its producer gate before waking a waiter");
        }

        const std::string close = extractFunction(content, "bool close() noexcept");
        if (close.empty()) {
            failures.push_back("failed to locate MPSC close");
        } else {
            requireContains(failures,
                            close,
                            "stream->control.gate.load(std::memory_order_seq_cst)",
                            "close must pair its cutoff with the producer seq_cst announcement");
            requireNotContains(failures,
                               close,
                               "stream->control.gate.store",
                               "close must not write the producer-owned in-flight flag");
            requireContains(failures,
                            close,
                            "m_closeState.store(CloseState::kClosed,\n"
                            "                           std::memory_order_seq_cst)",
                            "terminal close publication must share the waiter seq_cst order");
            requireOrdered(failures,
                           close,
                           "detachPublishedWaiter();",
                           "wakeDetachedWaiter",
                           "close must detach all channel-owned waiter state before waking");
        }

        const std::string closedAndDrained =
            extractFunction(content, "bool isClosedAndDrained() const noexcept");
        if (closedAndDrained.empty()) {
            failures.push_back("failed to locate MPSC isClosedAndDrained");
        } else {
            requireContains(failures,
                            closedAndDrained,
                            "m_closeState.load(std::memory_order_seq_cst)",
                            "waiter close recheck must share the waiter seq_cst order");
        }

        const std::string batchAwaitSuspend = extractFunction(
            content,
            "bool UnboundedRecvBatchAwaitable<T>::await_suspend(");
        if (batchAwaitSuspend.empty()) {
            failures.push_back("failed to locate MPSC vector batch await_suspend");
        } else {
            const size_t registration =
                batchAwaitSuspend.find("beginWaiterRegistration()");
            const size_t publication = batchAwaitSuspend.find("publishWaiter(");
            if (registration == std::string::npos ||
                publication == std::string::npos || registration >= publication) {
                failures.push_back(
                    "vector batch await_suspend must register before publishing waiter");
            } else {
                const std::string armingPath = batchAwaitSuspend.substr(
                    registration, publication - registration);
                requireNotContains(
                    failures,
                    armingPath,
                    "tryReceiveNow()",
                    "vector batch waiter arming path must not execute allocating receive");
                requireContains(
                    failures,
                    armingPath,
                    "hasPublishedValueForWaiter()",
                    "vector batch waiter arming path must use a non-allocating readiness check");
            }
        }

        const std::string pushReady =
            extractFunction(content, "void pushReadyStream(ProducerStream& stream) noexcept");
        if (pushReady.empty()) {
            failures.push_back("failed to locate MPSC ready stream publication");
        } else {
            requireContains(
                failures,
                pushReady,
                "std::memory_order_seq_cst",
                "first ready stream publication must join waiter discovery SC order");
        }

        const std::string appendReady =
            extractFunction(content, "void appendReadyStreams() noexcept");
        if (appendReady.empty()) {
            failures.push_back("failed to locate MPSC ready stream discovery");
        } else {
            requireContains(
                failures,
                appendReady,
                "m_readyStack.load(std::memory_order_seq_cst)",
                "ready stream probe must join waiter discovery SC order");
            requireContains(
                failures,
                appendReady,
                "m_readyStack.exchange(nullptr, std::memory_order_seq_cst)",
                "ready stream detach must join waiter discovery SC order");
        }

        const std::string waiterReadiness = extractFunction(
            content, "bool hasPublishedValueForWaiter() noexcept");
        if (waiterReadiness.empty()) {
            failures.push_back("failed to locate MPSC non-allocating waiter readiness check");
        } else {
            requireContains(
                failures,
                waiterReadiness,
                "appendReadyStreams();",
                "waiter readiness must discover first-active streams before probing data");
            requireContains(
                failures,
                waiterReadiness,
                "published.load(std::memory_order_seq_cst)",
                "waiter readiness must observe published data in the waiter SC order");
        }
    }

    if (!failures.empty()) {
        for (const std::string& failure : failures) {
            std::cerr << failure << '\n';
        }
        writer.addFailed();
        writer.writeResult();
        return 1;
    }

    writer.addPassed();
    writer.writeResult();
    std::cout << "t154_mpsc_unbounded_source PASS\n";
    return 0;
}
