/**
 * @file t57_awbld.cc
 * @brief 用途：验证新的组合式 Awaitable 公开表面存在且可用于强类型构建。
 * 关键覆盖点：`SequenceAwaitable`、`SequenceOps`、`SequenceStep`、`LocalSequenceStep`、`AwaitableBuilder`。
 * 通过条件：目标成功编译，静态断言成立，测试返回 0。
 */

#include "kernel/kernel/awaitable.h"
#include <expected>
#include <type_traits>

using namespace galay::kernel;

struct SurfaceFlow;
using SurfaceResult = std::expected<size_t, IOError>;
using SurfaceSequence = SequenceAwaitable<SurfaceResult, 4>;

struct SurfaceFlow {
    void onRecv(SequenceOps<SurfaceResult, 4>&, RecvIOContext&) {}
    void onLocal(SequenceOps<SurfaceResult, 4>&) {}
    void onFinish(SequenceOps<SurfaceResult, 4>&) {}
};

using SurfaceRecvStep = SequenceStep<SurfaceResult, 4, SurfaceFlow, RecvIOContext, &SurfaceFlow::onRecv>;
using SurfaceLocalStep = LocalSequenceStep<SurfaceResult, 4, SurfaceFlow, &SurfaceFlow::onLocal>;
using SurfaceBuilder = AwaitableBuilder<SurfaceResult, 4, SurfaceFlow>;

static_assert(std::is_constructible_v<SurfaceSequence, IOController*>);
static_assert(std::is_constructible_v<SurfaceRecvStep, SurfaceFlow*, char*, size_t>);
static_assert(std::is_constructible_v<SurfaceLocalStep, SurfaceFlow*>);
static_assert(std::is_constructible_v<SurfaceBuilder, IOController*, SurfaceFlow&>);

int main()
{
    return 0;
}
