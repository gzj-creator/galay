/**
 * @brief 用途：验证内联结果存储不会为 std::string 实例化无效的堆释放路径。
 * 关键覆盖点：Task<std::string> 的 return_value/takeResult 编译路径在 GCC 下不触发 -Wfree-nonheap-object。
 * 通过条件：测试目标可在开启 -Werror=free-nonheap-object 时完成编译并返回 0。
 */
#include <galay/cpp/galay-kernel/core/task.h>

#include <expected>
#include <string>

namespace
{

galay::kernel::Task<std::string> makeStringTask()
{
    co_return std::string("inline-result");
}

std::expected<std::string, galay::kernel::detail::TaskResultError> consumeStringTask(galay::kernel::Task<std::string>& task)
{
    return galay::kernel::detail::TaskAccess::takeResult(task);
}

} // namespace

int main()
{
    static_assert(galay::kernel::detail::TaskResultStorageTraits<std::string>::kInline,
                  "std::string should use inline task result storage on supported standard libraries");
    auto task = makeStringTask();
    auto task_ref = galay::kernel::detail::TaskAccess::taskRef(task);
    if (!task_ref.isValid() || task_ref.state()->m_handle == nullptr) {
        return 1;
    }

    task_ref.state()->m_handle.resume();
    auto result = consumeStringTask(task);
    return result.has_value() && *result == "inline-result" ? 0 : 2;
}
