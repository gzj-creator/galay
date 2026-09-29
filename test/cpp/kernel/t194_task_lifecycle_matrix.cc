/** @file t194_task_lifecycle_matrix.cc
 *  @brief Initial suspend, failure, result storage and last-owner destruction.
 */
#include <galay/cpp/galay-kernel/core/task.h>
#include <array>
#include <atomic>
#include <iostream>
#include <thread>
using namespace galay::kernel;
namespace {
struct Probe {
    std::atomic<int>* destroyed;
    Probe(std::atomic<int>& count) : destroyed(&count) {}
    Probe(Probe&& other) noexcept : destroyed(std::exchange(other.destroyed, nullptr)) {}
    Probe(const Probe&) = delete;
    ~Probe() { if (destroyed) { destroyed->fetch_add(1); } }
};
struct Large { std::array<int, 128> values{}; };
template <class T, size_t Payload>
Task<T> create(Probe probe, std::atomic<int>& entered, bool suspend) {
    // volatile keeps the payload live across suspension in this reproduction.
    volatile std::byte payload[Payload]{};
    ++entered;
    if (suspend) { co_await std::suspend_always{}; }
    payload[Payload - 1] = std::byte{7};
    if constexpr (std::is_void_v<T>) { co_return; }
    else if constexpr (std::is_same_v<T, int>) { co_return 7; }
    else { Large value; value.values.back() = 7; co_return value; }
}
template <class T, size_t Payload>
bool verify() {
    for (int path = 0; path < 6; ++path) {
        std::atomic<int> destroyed{0}, entered{0};
        if (path == 4) { detail::setFrameAllocationFailureForTesting(true); }
        if (path == 5) { detail::setTaskStateAllocationFailureForTesting(true); }
        auto task = create<T, Payload>(Probe(destroyed), entered, path == 2);
        detail::setFrameAllocationFailureForTesting(false);
        detail::setTaskStateAllocationFailureForTesting(false);
        if (entered != 0 || (path >= 4 ? task.isValid() : !task.isValid())) { return false; }
        if (path >= 4) {
            if (destroyed != 1) { return false; }
            continue;
        }
        TaskRef owner = detail::TaskAccess::detachTask(std::move(task));
        if (path == 1 || path == 2 || path == 3) {
            owner.state()->m_handle.resume();
            if (entered != 1) { return false; }
            if (path == 2) {
                if (owner.state()->m_done || destroyed != 0) { return false; }
            } else {
                if (!owner.state()->m_done || owner.state()->m_handle || destroyed != 1) { return false; }
                if constexpr (std::is_same_v<T, Large>) {
                    if (owner.state()->m_result_kind != TaskState::ResultStorageKind::Heap) { return false; }
                } else if constexpr (std::is_same_v<T, int>) {
                    if (owner.state()->m_result_kind != TaskState::ResultStorageKind::Inline) { return false; }
                }
            }
        }
        // Includes both unfinished frame and completed heap-result last releases.
        std::thread releaser([ref = std::move(owner)]() mutable { ref = TaskRef{}; });
        releaser.join();
        if (destroyed != 1) { return false; }
    }
    return true;
}
}
int main() {
    const bool ok = verify<void, 64>() && verify<int, 64>() && verify<Large, 64>() &&
                    verify<void, 512>() && verify<int, 512>() && verify<Large, 4096>();
    if (!ok) { std::cerr << "T194 task lifecycle matrix failed\n"; return 1; }
    std::cout << "T194 task lifecycle matrix PASS\n";
    return 0;
}
