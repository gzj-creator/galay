/**
 * @file t19_v2_stdio_client_concurrency.cc
 * @brief v2 stdio client non-blocking concurrent request boundary.
 */

#include <galay/cpp/galay-mcp/v2/client.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <streambuf>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

class GatedInputBuf final : public std::streambuf {
public:
    GatedInputBuf(std::string data, std::atomic<bool>& entered,
                  std::atomic<bool>& release)
        : m_data(std::move(data)), m_entered(&entered), m_release(&release)
    {
    }

protected:
    int_type underflow() override
    {
        if (gptr() != nullptr && gptr() < egptr()) return traits_type::to_int_type(*gptr());
        if (m_offset >= m_data.size()) return traits_type::eof();
        m_entered->store(true, std::memory_order_release);
        while (!m_release->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        char* begin = m_data.data() + m_offset;
        setg(begin, begin, m_data.data() + m_data.size());
        m_offset = m_data.size();
        return traits_type::to_int_type(*gptr());
    }

private:
    std::string m_data;
    std::atomic<bool>* m_entered;
    std::atomic<bool>* m_release;
    std::size_t m_offset = 0;
};

} // namespace

int main()
{
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> secondDone{false};
    GatedInputBuf inputBuffer(
        R"({"jsonrpc":"2.0","id":1,"result":{"resultType":"complete","ttlMs":0,"cacheScope":"private","supportedVersions":["2026-07-28"],"capabilities":{}}}
{"jsonrpc":"2.0","id":2,"result":{"resultType":"complete","ttlMs":0,"cacheScope":"private","supportedVersions":["2026-07-28"],"capabilities":{}}}
{"jsonrpc":"2.0","id":3,"result":{"resultType":"complete","ttlMs":0,"cacheScope":"private","supportedVersions":["2026-07-28"],"capabilities":{}}}
)",
        entered, release);
    std::istream input(&inputBuffer);
    std::ostringstream output;
    galay::mcp::v2::McpStdioClient client(input, output);

    std::expected<galay::mcp::v2::DiscoverResult, galay::mcp::McpError> firstResult;
    std::expected<galay::mcp::v2::DiscoverResult, galay::mcp::McpError> secondResult =
        std::unexpected(galay::mcp::McpError::invalidResponse("pending"));

    std::thread first([&] {
        firstResult = client.discover();
    });
    const auto enteredDeadline = std::chrono::steady_clock::now() + 1s;
    while (!entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < enteredDeadline) {
        std::this_thread::yield();
    }
    if (!entered.load(std::memory_order_acquire)) {
        release.store(true, std::memory_order_release);
        first.join();
        std::cerr << "first stdio request did not reach the input boundary\n";
        return 1;
    }

    std::thread second([&] {
        secondResult = client.discover();
        secondDone.store(true, std::memory_order_release);
    });
    const auto nonBlockingDeadline = std::chrono::steady_clock::now() + 100ms;
    while (!secondDone.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < nonBlockingDeadline) {
        std::this_thread::yield();
    }
    const bool secondReturned = secondDone.load(std::memory_order_acquire);
    release.store(true, std::memory_order_release);
    first.join();
    second.join();

    if (!secondReturned || !firstResult.has_value() || secondResult.has_value() ||
        secondResult.error().code() != galay::mcp::McpErrorCode::Overload) {
        std::cerr << "stdio concurrent request was not rejected without blocking"
                  << " second_returned=" << secondReturned
                  << " first_ok=" << firstResult.has_value()
                  << " second_ok=" << secondResult.has_value()
                  << "\n";
        return 1;
    }
    auto reusableResult = client.discover();
    if (!reusableResult) {
        std::cerr << "stdio request gate was not released after completion\n";
        return 1;
    }
    std::cout << "T19-V2StdioClientConcurrency PASS\n";
    return 0;
}
