#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#define private public
#include <galay/cpp/galay-utils/cache/lru_cache.hpp>
#undef private

namespace {

struct ManualClock {
    using rep = int64_t;
    using period = std::milli;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<ManualClock>;

    static constexpr bool is_steady = true;
    static inline time_point current{duration{0}};

    static time_point now() noexcept
    {
        return current;
    }

    static void reset()
    {
        current = time_point{duration{0}};
    }

    static void advance(duration delta)
    {
        current += delta;
    }
};

enum class ReadError {
    kOpen,
    kRead,
    kClose,
    kPath,
};

std::expected<std::string, ReadError> repoRoot()
{
    std::string path(__FILE__);
    const std::string marker = "/test/cpp/utils/";
    const size_t marker_pos = path.rfind(marker);
    if (marker_pos == std::string::npos) {
        return std::unexpected(ReadError::kPath);
    }
    path.resize(marker_pos);
    return path;
}

std::expected<std::string, ReadError> readFile(const std::string& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return std::unexpected(ReadError::kOpen);
    }

    std::string content;
    std::vector<char> buffer(64 * 1024);
    while (true) {
        const ssize_t bytes_read = ::read(fd, buffer.data(), buffer.size());
        if (bytes_read < 0) {
            const int close_result = ::close(fd);
            if (close_result != 0) {
                std::cerr << "close after read failure also failed\n";
            }
            return std::unexpected(ReadError::kRead);
        }
        if (bytes_read == 0) {
            break;
        }
        std::string& appended = content.append(buffer.data(), static_cast<size_t>(bytes_read));
        if (&appended != &content) {
            const int close_result = ::close(fd);
            if (close_result != 0) {
                std::cerr << "close after append failure also failed\n";
            }
            return std::unexpected(ReadError::kRead);
        }
    }

    const int close_result = ::close(fd);
    if (close_result != 0) {
        return std::unexpected(ReadError::kClose);
    }
    return content;
}

std::expected<std::string, ReadError> repoFile(std::string_view relative_path)
{
    auto root = repoRoot();
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    std::string path = *root;
    std::string& slash_appended = path.append("/");
    if (&slash_appended != &path) {
        return std::unexpected(ReadError::kPath);
    }
    std::string& file_appended = path.append(relative_path.data(), relative_path.size());
    if (&file_appended != &path) {
        return std::unexpected(ReadError::kPath);
    }
    return readFile(path);
}

int testLruHotKeyExpirationHeap()
{
    using Cache = galay::utils::LruCache<std::string,
                                         int,
                                         std::hash<std::string>,
                                         std::equal_to<std::string>,
                                         ManualClock>;

    ManualClock::reset();
    Cache cache(2, ManualClock::duration{1000}, nullptr,
                Cache::ExpirationPolicy::ExpireAfterAccess);

    const bool inserted = cache.put("alpha", 1);
    if (!inserted) {
        std::cerr << "failed to insert alpha\n";
        return 1;
    }

    for (size_t i = 0; i < 128; ++i) {
        ManualClock::advance(ManualClock::duration{1});
        int* value = cache.get("alpha");
        if (value == nullptr || *value != 1) {
            std::cerr << "hot key should stay present during access refresh\n";
            return 1;
        }
    }

    if (cache.m_expirations.size() > 4) {
        std::cerr << "hot key refresh should not grow expiration heap, size="
                  << cache.m_expirations.size() << "\n";
        return 1;
    }
    return 0;
}

int testConsistentHashMemoryOrderSource()
{
    auto source = repoFile("src/cpp/galay-utils/algorithm/consistent_hash.hpp");
    if (!source.has_value()) {
        std::cerr << "failed to read consistent_hash.hpp\n";
        return 1;
    }

    if (source->find("m_state.load(std::memory_order_acquire)") == std::string::npos) {
        std::cerr << "consistent hash snapshot loads must use acquire order\n";
        return 1;
    }
    if (source->find("std::memory_order_release") == std::string::npos) {
        std::cerr << "consistent hash snapshot publication must use release order\n";
        return 1;
    }
    if (source->find("std::memory_order_seq_cst") != std::string::npos) {
        std::cerr << "consistent hash must not use seq_cst on snapshot atomics\n";
        return 1;
    }
    return 0;
}

} // namespace

int main()
{
    if (const int rc = testLruHotKeyExpirationHeap()) {
        return rc;
    }
    if (const int rc = testConsistentHashMemoryOrderSource()) {
        return rc;
    }
    std::cout << "T18-CacheHashPerfBoundaries PASS\n";
    return 0;
}
