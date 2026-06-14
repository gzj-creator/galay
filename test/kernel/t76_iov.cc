/**
 * @file t76_iov.cc
 * @brief 用途：锁定 AwaitableBuilder `readv/writev` 与状态机 iovec 动作公开表面。
 * 关键覆盖点：`MachineSignal::kWaitReadv/kWaitWritev`、
 * `MachineAction::waitReadv/waitWritev`、
 * `AwaitableBuilder::readv/writev` 的 `std::array` 与 C 数组借用入口。
 * 通过条件：目标成功编译，静态断言成立，测试返回 0。
 */

#include "kernel/kernel/awaitable.h"
#include <array>
#include <concepts>
#include <expected>
#include <sys/uio.h>
#include <type_traits>

using namespace galay::kernel;

using SurfaceResult = std::expected<size_t, IOError>;

namespace {

struct SurfaceFlow {
    void onReadv(SequenceOps<SurfaceResult, 4>& ops, ReadvIOContext& ctx) {
        ops.complete(std::move(ctx.m_result));
    }

    void onWritev(SequenceOps<SurfaceResult, 4>& ops, WritevIOContext& ctx) {
        ops.complete(std::move(ctx.m_result));
    }
};

using SurfaceBuilder = AwaitableBuilder<SurfaceResult, 4, SurfaceFlow>;
using BorrowedIovecArray = struct iovec[2];

template <typename BuilderT>
concept HasArrayReadv = requires(BuilderT& builder, std::array<struct iovec, 2>& iovecs) {
    { builder.template readv<&SurfaceFlow::onReadv>(iovecs, iovecs.size()) } -> std::same_as<BuilderT&>;
};

template <typename BuilderT>
concept HasArrayWritev = requires(BuilderT& builder, std::array<struct iovec, 2>& iovecs) {
    { builder.template writev<&SurfaceFlow::onWritev>(iovecs, iovecs.size()) } -> std::same_as<BuilderT&>;
};

template <typename BuilderT>
concept HasBorrowedReadv = requires(BuilderT& builder, BorrowedIovecArray& iovecs) {
    { builder.template readv<&SurfaceFlow::onReadv>(iovecs, 2) } -> std::same_as<BuilderT&>;
};

template <typename BuilderT>
concept HasBorrowedWritev = requires(BuilderT& builder, BorrowedIovecArray& iovecs) {
    { builder.template writev<&SurfaceFlow::onWritev>(iovecs, 2) } -> std::same_as<BuilderT&>;
};

}  // namespace

static_assert(std::same_as<decltype(MachineSignal::kWaitReadv), MachineSignal>);
static_assert(std::same_as<decltype(MachineSignal::kWaitWritev), MachineSignal>);
static_assert(std::is_same_v<
              decltype(MachineAction<SurfaceResult>::waitReadv(static_cast<const struct iovec*>(nullptr), 0)),
              MachineAction<SurfaceResult>>);
static_assert(std::is_same_v<
              decltype(MachineAction<SurfaceResult>::waitWritev(static_cast<const struct iovec*>(nullptr), 0)),
              MachineAction<SurfaceResult>>);
static_assert(HasArrayReadv<SurfaceBuilder>);
static_assert(HasArrayWritev<SurfaceBuilder>);
static_assert(HasBorrowedReadv<SurfaceBuilder>);
static_assert(HasBorrowedWritev<SurfaceBuilder>);

int main() {
    return 0;
}
