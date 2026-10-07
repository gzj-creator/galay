/**
 * @file operation_key.hpp
 * @brief 异步 operation 的纯值标识；不负责分配、查表或对象保活。
 */
#ifndef GALAY_KERNEL_OPERATION_KEY_HPP
#define GALAY_KERNEL_OPERATION_KEY_HPP

#include <cstdint>

namespace galay::kernel {

/**
 * @brief Owner 注册表中的 slot + generation。
 * @note generation == 0 无效；slot == 0 可用。注册表必须在访问 operation
 *       前校验 generation，并在 generation 溢出前 drain/rekey；此值本身
 *       既不是稳定指针，也不保证 backend event 已经过校验。
 */
struct OperationKey {
    uint32_t slot = 0;
    uint32_t generation = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return generation != 0; }
    friend constexpr bool operator==(OperationKey, OperationKey) noexcept = default;
};

static_assert(sizeof(OperationKey) == sizeof(uint64_t));

} // namespace galay::kernel
#endif // GALAY_KERNEL_OPERATION_KEY_HPP
