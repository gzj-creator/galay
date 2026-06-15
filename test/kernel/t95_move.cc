/**
 * @file t95_move.cc
 * @brief 用途：锁定 IOController 只能移动，不能复制。
 * 关键覆盖点：复制构造/赋值禁用，移动构造/赋值保留。
 * 通过条件：相关 static_assert 全部成立。
 */

#include "galay-kernel/core/io_controller.hpp"

#include <type_traits>
#include <iostream>

using galay::kernel::IOController;

static_assert(!std::is_copy_constructible_v<IOController>);
static_assert(!std::is_copy_assignable_v<IOController>);
static_assert(std::is_move_constructible_v<IOController>);
static_assert(std::is_move_assignable_v<IOController>);

int main() {
    std::cout << "T95-IOControllerMoveOnlySurface PASS\n";
    return 0;
}
