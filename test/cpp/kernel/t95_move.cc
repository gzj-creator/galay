/**
 * @file t95_move.cc
 * @brief 用途：锁定 IOController 只能由稳定地址 owner 持有，不能复制或移动。
 * 关键覆盖点：复制构造/赋值和移动构造/赋值全部禁用。
 * 通过条件：相关 static_assert 全部成立。
 */

#include <galay/cpp/galay-kernel/core/io_controller.hpp>

#include <type_traits>
#include <iostream>

using galay::kernel::IOController;

static_assert(!std::is_copy_constructible_v<IOController>);
static_assert(!std::is_copy_assignable_v<IOController>);
static_assert(!std::is_move_constructible_v<IOController>);
static_assert(!std::is_move_assignable_v<IOController>);

int main() {
    std::cout << "T95-IOControllerStableAddressSurface PASS\n";
    return 0;
}
