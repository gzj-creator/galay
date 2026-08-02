/**
 * @file t169_iouring_connect_eisconn.cc
 * @brief 验证 io_uring connect 将 EISCONN 视为已连接成功。
 */

#include <galay/cpp/galay-kernel/core/io_handlers.hpp>

#include <cerrno>
#include <iostream>

int main()
{
#ifdef USE_IOURING
    io_uring_cqe cqe{};
    cqe.res = -EISCONN;
    const auto result = galay::kernel::io::handleConnect(&cqe);
    if (!result) {
        std::cerr << "io_uring connect rejected EISCONN: "
                  << result.error().message() << '\n';
        return 1;
    }
#endif
    return 0;
}
