#ifdef NDEBUG
#undef NDEBUG
#endif

#include <galay/cpp/galay-utils/system/rdtsc.hpp>
#include <galay/cpp/galay-utils/system/cpu.hpp>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <system_error>
#include <thread>
#include <type_traits>

#if defined(__linux__) && defined(__x86_64__)
#include <sys/prctl.h>
#endif

namespace {

void test_conversion(const galay::utils::Rdtsc& counter) {
    const auto zero = counter.to_nanoseconds(0);
    assert(zero && zero->count() == 0);

    const auto one_second = counter.to_nanoseconds(counter.frequency_hz());
    assert(one_second);
    assert(one_second->count() >= 1'000'000'000 -
           static_cast<std::int64_t>((counter.frequency_hz() >> 32U) + 1));
    assert(one_second->count() <= 1'000'000'000);

    const auto sixty_seconds = counter.to_nanoseconds(counter.frequency_hz() * 60);
    assert(sixty_seconds);
    assert(sixty_seconds->count() >= 60'000'000'000 -
           static_cast<std::int64_t>(((counter.frequency_hz() * 60) >> 32U) + 1));
    assert(sixty_seconds->count() <= 60'000'000'000);

#if defined(__SIZEOF_INT128__)
    using Wide = unsigned __int128;
    const std::uint64_t ticks[] = {1, 31, 123456, counter.frequency_hz() * 100,
                                   std::numeric_limits<std::uint64_t>::max()};
    for (const auto value : ticks) {
        const auto actual = counter.to_nanoseconds(value);
        const Wide exact = static_cast<Wide>(value) * 1'000'000'000 / counter.frequency_hz();
        const Wide error_bound = (static_cast<Wide>(value) >> 32U) + 1;
        if (exact <= static_cast<Wide>(std::numeric_limits<std::int64_t>::max())) {
            assert(actual);
            assert(static_cast<Wide>(actual->count()) <= exact);
            assert(exact - static_cast<Wide>(actual->count()) <= error_bound);
        } else if (exact - error_bound > static_cast<Wide>(std::numeric_limits<std::int64_t>::max())) {
            assert(!actual && actual.error() == std::errc::value_too_large);
        }
    }
#endif

    if (counter.frequency_hz() <= 1'000'000'000) {
        const auto overflow = counter.to_nanoseconds(std::numeric_limits<std::uint64_t>::max());
        assert(!overflow);
        assert(overflow.error() == std::errc::value_too_large);
    }
}

void test_access_error() {
#if defined(__linux__) && defined(__x86_64__)
    const int disabled = prctl(PR_SET_TSC, PR_TSC_SIGSEGV);
    assert(disabled == 0);
    const auto denied = galay::utils::Rdtsc::create();
    // 在断言或可能读取 TSC 的库代码执行前恢复权限，避免触发访问陷阱。
    const int restored = prctl(PR_SET_TSC, PR_TSC_ENABLE);
    assert(restored == 0);
    assert(!denied && denied.error() == std::errc::operation_not_permitted);
#endif
}

void test_reads(const galay::utils::Rdtsc& counter) {
    auto previous = counter.read_end();
    for (std::size_t index = 0; index < 100'000; ++index) {
        const auto current = counter.read_start();
        assert(current >= previous);
        previous = current;
    }
    const auto relaxed = counter.read_relaxed();
    assert(relaxed >= previous);

    const auto reference_start = std::chrono::steady_clock::now();
    const auto start = counter.read_start();
    std::this_thread::sleep_for(std::chrono::milliseconds{50});
    const auto end = counter.read_end();
    const auto reference_end = std::chrono::steady_clock::now();
    assert(end > start);
    const auto elapsed = counter.to_nanoseconds(end - start);
    assert(elapsed);
    const auto reference = std::chrono::duration_cast<std::chrono::nanoseconds>(
        reference_end - reference_start);
    const auto difference = reference > *elapsed ? reference - *elapsed : *elapsed - reference;
    assert(difference < reference / 200 + std::chrono::microseconds{50});
}

} // namespace

int main() {
    static_assert(!std::is_default_constructible_v<galay::utils::Rdtsc>);
#if defined(__linux__)
    const auto inherited = galay::utils::CPU::cpu_affinity();
    assert(inherited && !inherited->empty());
    const unsigned cpu = inherited->front();
    const auto pinned = galay::utils::CPU::bind_current_thread(std::span{&cpu, 1});
    assert(pinned && pinned->size() == 1 && pinned->front() == cpu);
#endif
    const auto counter = galay::utils::Rdtsc::create();
    if (!galay::utils::Rdtsc::is_supported) {
        assert(!counter && counter.error() == std::errc::operation_not_supported);
        return 0;
    }
    assert(counter);
    assert(counter->frequency_hz() > 0);
    test_conversion(*counter);
    test_reads(*counter);
    test_access_error();
    std::cout << "RDTSC tests passed, frequency_hz=" << counter->frequency_hz() << '\n';
    return 0;
}
