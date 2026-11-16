/**
 * @file t207_timer_wheel_geometry.cc
 * @brief 验证单线程与多线程时间轮共用的五层几何计算。
 */

#include <galay/cpp/galay-kernel/common/timer_manager.hpp>
#include <galay/cpp/galay-kernel/common/timer_manager_mt.hpp>
#include <galay/cpp/galay-kernel/common/timer_wheel.hpp>

#include <cstdint>
#include <iostream>

namespace {

using TimingWheel = galay::kernel::detail::TimerWheelGeometry;

static_assert(TimingWheel::kWheel1Size == 256);
static_assert(TimingWheel::kWheel1Span == 256);
static_assert(TimingWheel::kWheel2Span == 16'384);
static_assert(TimingWheel::kWheel3Span == 1'048'576);
static_assert(TimingWheel::kWheel4Span == 67'108'864);
static_assert(TimingWheel::kWheel5Span == 4'294'967'296);

static_assert(TimingWheel::layer_for_delay(0) == 0);
static_assert(TimingWheel::layer_for_delay(255) == 0);
static_assert(TimingWheel::layer_for_delay(256) == 1);
static_assert(TimingWheel::layer_for_delay(16'383) == 1);
static_assert(TimingWheel::layer_for_delay(16'384) == 2);
static_assert(TimingWheel::layer_for_delay(1'048'575) == 2);
static_assert(TimingWheel::layer_for_delay(1'048'576) == 3);
static_assert(TimingWheel::layer_for_delay(67'108'863) == 3);
static_assert(TimingWheel::layer_for_delay(67'108'864) == 4);

static_assert(TimingWheel::slot_index(0, 0x1ff) == 0xff);
static_assert(TimingWheel::slot_index(1, 0x1ff) == 1);
static_assert(TimingWheel::slot_index(2, 0x3fff) == 0);
static_assert(TimingWheel::slot_index(3, 0xfffff) == 0);
static_assert(TimingWheel::slot_index(4, 0x3ffffff) == 0);

static_assert(!TimingWheel::is_cascade_tick(0, 0));
static_assert(TimingWheel::is_cascade_tick(0, 256));
static_assert(!TimingWheel::is_cascade_tick(0, 257));
static_assert(TimingWheel::is_cascade_tick(1, 16'384));
static_assert(TimingWheel::is_cascade_tick(2, 1'048'576));
static_assert(TimingWheel::is_cascade_tick(3, 67'108'864));

static_assert(TimingWheel::expiry_tick(100, 25) == 125);
static_assert(TimingWheel::floor_ticks(1'999, 1'000) == 1);
static_assert(TimingWheel::ceil_ticks(1'000, 1'000) == 1);
static_assert(TimingWheel::ceil_ticks(1'001, 1'000) == 2);

static_assert(galay::kernel::TimingWheelTimerManager::WHEEL5_SPAN ==
              TimingWheel::kWheel5Span);
static_assert(galay::kernel::ThreadSafeTimerManager::WHEEL5_SPAN ==
              TimingWheel::kWheel5Span);

} // namespace

int main()
{
    std::cout << "T207-TimerWheelGeometry PASS\n";
    return 0;
}
