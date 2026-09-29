// Standalone container experiment; no epoll syscall or production throughput claim.
#include <absl/container/flat_hash_map.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <unordered_map>
#include <vector>

namespace {

// Match the measured controller stride; pointees remain stable throughout a run.
struct alignas(8) Controller {
    std::array<std::byte, 72> payload{};
};
static_assert(sizeof(Controller) == 72);

struct RegistrationEntry {
    Controller* controller;
};

struct PendingChange {
    RegistrationEntry* entry;
    std::uint32_t events;
};

using StandardIndex = std::unordered_map<Controller*, std::size_t>;
using FlatIndex = absl::flat_hash_map<Controller*, std::size_t>;

template <typename Index>
std::optional<std::uint64_t> cycle(Index& index, std::vector<PendingChange>& pending,
                                 std::vector<RegistrationEntry>& entries,
                                 std::size_t width, std::size_t rounds) {
    std::uint64_t checksum = 0;
    for (std::size_t round = 0; round < rounds; ++round) {
        for (std::size_t i = 0; i < width; ++i) {
            auto* key = entries[i].controller;
            if (index.find(key) != index.end()) { return std::nullopt; }
            index[key] = pending.size();
            pending.push_back({&entries[i], 1});
        }
        for (std::size_t i = 0; i < width; ++i) {
            // All widths are powers of two; 17 visits every key once.
            auto* key = entries[(i * 17) % width].controller;
            const auto found = index.find(key);
            if (found == index.end() || found->second >= pending.size() ||
                pending[found->second].entry->controller != key) { return std::nullopt; }
            checksum += found->second + 1;
            pending[found->second].events = 2;
            if (index.find(entries[width + i].controller) != index.end()) {
                return std::nullopt;
            }
        }
        while (!pending.empty()) {
            // Exercise both the flush-at-front and middle-cancellation paths.
            const std::size_t position = round % 2 == 0 ? 0 : pending.size() / 2;
            const auto change = pending[position];
            const auto found = index.find(change.entry->controller);
            if (found == index.end() || found->second != position || change.events != 2) {
                return std::nullopt;
            }
            checksum += change.events;
            if (index.erase(change.entry->controller) != 1) { return std::nullopt; }
            if (position != pending.size() - 1) {
                pending[position] = pending.back();
                index[pending[position].entry->controller] = position;
            }
            pending.pop_back();
        }
        if (!index.empty()) { return std::nullopt; }
    }
    return checksum;
}

template <typename Index>
bool measure(const char* name, std::size_t width, int repetition, bool reserve) {
    std::vector<Controller> controllers(2 * width);
    std::vector<RegistrationEntry> entries;
    for (auto& controller : controllers) { entries.push_back({&controller}); }
    std::vector<PendingChange> pending;
    Index index;
    if (reserve) { index.reserve(width); }
    constexpr std::size_t warmup_rounds = 16;
    const std::uint64_t per_round = width * (width + 1) / 2 + 2 * width;
    const auto warmup = cycle(index, pending, entries, width, warmup_rounds);
    if (!warmup || *warmup != per_round * warmup_rounds) { return false; }

    const std::size_t rounds = 1'048'576 / width;
    const auto begin = std::chrono::steady_clock::now();
    const auto checksum = cycle(index, pending, entries, width, rounds);
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    if (!checksum || *checksum != per_round * rounds) { return false; }
    const double ns = std::chrono::duration<double, std::nano>(elapsed).count() /
                      static_cast<double>(rounds * width);
    if (ns <= 0) { return false; }
    std::cout << name << ',' << repetition << ',' << width << ',' << rounds << ','
              << std::fixed << std::setprecision(3) << ns << ',' << *checksum << '\n';
    return true;
}

} // namespace

int main() {
    std::cout << "variant,run,width,rounds,ns_per_lifecycle,checksum\n";
    for (int repetition = 1; repetition <= 5; ++repetition) {
        for (const std::size_t width : {1U, 8U, 32U, 256U}) {
            // Alternate order to avoid always measuring one candidate last.
            for (int slot = 0; slot < 3; ++slot) {
                const int candidate = repetition % 2 == 0 ? 2 - slot : slot;
                const bool ok = candidate == 0
                    ? measure<StandardIndex>("std", width, repetition, false)
                    : candidate == 1
                        ? measure<StandardIndex>("std_reserved", width, repetition, true)
                        : measure<FlatIndex>("absl_flat", width, repetition, false);
                if (!ok) {
                    std::cerr << "pending index lifecycle validation failed: width=" << width
                              << " candidate=" << candidate << '\n';
                    return 1;
                }
            }
        }
    }
    return 0;
}
