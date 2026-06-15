/**
 * @file t44_waitreg.cc
 * @brief 用途：验证 Channel 等待注册状态的 `armed`、`clear` 与重复信号边界。
 * 关键覆盖点：等待注册、重复 signal 防抖、clear 或 re-arm 之后的重新等待。
 * 通过条件：等待注册状态转换全部正确，测试返回 0。
 */

#include "galay-kernel/core/wait_registration.h"

#include <cstdint>
#include <iostream>

using namespace galay::kernel;

int main() {
    WaitRegistration registration;

    void* first_waiter = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1));
    void* second_waiter = reinterpret_cast<void*>(static_cast<uintptr_t>(0x2));

    if (!registration.arm(first_waiter)) {
        std::cerr << "[T44] failed to arm first waiter\n";
        return 1;
    }

    if (!registration.hasWaiter()) {
        std::cerr << "[T44] expected armed waiter\n";
        return 1;
    }

    if (registration.generation() != 1) {
        std::cerr << "[T44] expected generation=1, got " << registration.generation() << "\n";
        return 1;
    }

    void* signaled = registration.consumeWake();
    if (signaled != first_waiter) {
        std::cerr << "[T44] expected first waiter to be signaled\n";
        return 1;
    }

    if (registration.consumeWake() != nullptr) {
        std::cerr << "[T44] consumeWake should be single-shot\n";
        return 1;
    }

    if (registration.clear(first_waiter)) {
        std::cerr << "[T44] clear should fail after consumeWake\n";
        return 1;
    }

    if (!registration.arm(second_waiter)) {
        std::cerr << "[T44] failed to re-arm second waiter\n";
        return 1;
    }

    if (registration.generation() != 2) {
        std::cerr << "[T44] expected generation=2 after re-arm, got "
                  << registration.generation() << "\n";
        return 1;
    }

    if (!registration.clear(second_waiter)) {
        std::cerr << "[T44] clear should succeed for currently armed waiter\n";
        return 1;
    }

    if (registration.hasWaiter()) {
        std::cerr << "[T44] expected waiter to be cleared\n";
        return 1;
    }

    std::cout << "T44-ChannelWaitRegistration PASS\n";
    return 0;
}
