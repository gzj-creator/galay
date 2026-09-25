/** @brief IOController 槽位绑定只依赖自身头，不依赖调度器实现。 */
#include <galay/cpp/galay-kernel/core/io_controller.hpp>
#include <cassert>
#include <cstddef>
#include <iostream>

int main()
{
    using namespace galay::kernel;
    IOController controller(GHandle{.fd = -1});
    alignas(std::max_align_t) std::byte read_token{};
    alignas(std::max_align_t) std::byte write_token{};
    assert(controller.fillAwaitable(RECV, &read_token));
    assert(controller.fillAwaitable(SEND, &write_token));
    assert(controller.m_awaitable[IOController::READ] == &read_token);
    assert(controller.m_awaitable[IOController::WRITE] == &write_token);
    controller.removeAwaitable(RECV);
    assert(controller.m_awaitable[IOController::READ] == nullptr);
    assert(controller.m_awaitable[IOController::WRITE] == &write_token);
    controller.removeAwaitable(SEND);
    assert(controller.m_awaitable[IOController::WRITE] == nullptr);
    assert(!controller.fillAwaitable(INVALID, nullptr));
    std::cout << "T184-IOControllerSelfContained PASS\n";
}
