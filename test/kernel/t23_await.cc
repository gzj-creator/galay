/**
 * @file t23_await.cc
 * @brief 用途：验证 Awaitable 结果按值返回与按引用返回的性能差异。
 * 关键覆盖点：热路径 await 开销、不同返回策略的成本对比、基准数据输出。
 * 通过条件：所有测量轮次顺利完成并输出对比结果，程序返回 0。
 */

#include <iostream>
#include <chrono>
#include <atomic>
#include <concepts>
#include <type_traits>
#include <utility>

// 模拟 Awaitable 对象
struct MockResult {
    int code = 0;
    int data = 0;
};

// 方案1: 返回对象
class AwaitableByValue {
public:
    explicit AwaitableByValue(int* counter) : m_counter(counter) {}

    bool await_ready() const noexcept { return false; }
    bool await_suspend(void*) noexcept { return true; }
    MockResult await_resume() noexcept {
        ++(*m_counter);
        return m_result;
    }

private:
    int* m_counter;
    MockResult m_result;
};

class ObjectByValue {
public:
    AwaitableByValue lock() {
        return AwaitableByValue(&m_counter);
    }
    int getCounter() const { return m_counter; }
private:
    int m_counter = 0;
};

// 方案2: 返回引用
class AwaitableByRef {
public:
    explicit AwaitableByRef(int* counter) : m_counter(counter) {}

    bool await_ready() const noexcept { return false; }
    bool await_suspend(void*) noexcept { return true; }
    MockResult await_resume() noexcept {
        ++(*m_counter);
        auto result = m_result;
        reset();  // 重置状态
        return result;
    }

    void reset() noexcept {
        m_result = MockResult{};
    }

private:
    int* m_counter;
    MockResult m_result;
};

class ObjectByRef {
public:
    ObjectByRef() : m_awaitable(&m_counter) {}

    AwaitableByRef& lock() {
        return m_awaitable;
    }
    int getCounter() const { return m_counter; }
private:
    int m_counter = 0;
    AwaitableByRef m_awaitable;
};

// 方案3: 返回引用但不重置 (最小开销)
class AwaitableByRefNoReset {
public:
    explicit AwaitableByRefNoReset(int* counter) : m_counter(counter) {}

    bool await_ready() const noexcept { return false; }
    bool await_suspend(void*) noexcept { return true; }
    MockResult await_resume() noexcept {
        ++(*m_counter);
        return m_result;
    }

private:
    int* m_counter;
    MockResult m_result;
};

class ObjectByRefNoReset {
public:
    ObjectByRefNoReset() : m_awaitable(&m_counter) {}

    AwaitableByRefNoReset& lock() {
        return m_awaitable;
    }
    int getCounter() const { return m_counter; }
private:
    int m_counter = 0;
    AwaitableByRefNoReset m_awaitable;
};

template <typename T>
using LockRef = decltype(std::declval<T&>().lock());

template <typename T>
concept BenchmarkTarget =
    std::default_initializable<T> &&
    std::is_lvalue_reference_v<LockRef<T>> &&
    requires(T t) {
        { t.getCounter() } -> std::convertible_to<int>;
    } &&
    requires(std::remove_reference_t<LockRef<T>> awaitable) {
        { awaitable.await_ready() } -> std::convertible_to<bool>;
        { awaitable.await_suspend(nullptr) } -> std::convertible_to<bool>;
        awaitable.await_resume();
    };

template<typename T>
void benchmark(const char* name, int iterations) {
    static_assert(BenchmarkTarget<T>,
                  "benchmark<T> requires lock() returning lvalue ref and awaitable methods");
    T obj;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        auto& awaitable = obj.lock();
        awaitable.await_ready();
        awaitable.await_suspend(nullptr);
        volatile auto result = awaitable.await_resume();
        (void)result;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << name << ": " << ns_per_op << " ns/op"
              << " (counter=" << obj.getCounter() << ")" << std::endl;
}

// 特化版本处理返回值类型
template<>
void benchmark<ObjectByValue>(const char* name, int iterations) {
    ObjectByValue obj;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < iterations; ++i) {
        auto awaitable = obj.lock();  // 返回值，不是引用
        awaitable.await_ready();
        awaitable.await_suspend(nullptr);
        volatile auto result = awaitable.await_resume();
        (void)result;
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

    double ns_per_op = static_cast<double>(duration.count()) / iterations;

    std::cout << name << ": " << ns_per_op << " ns/op"
              << " (counter=" << obj.getCounter() << ")" << std::endl;
}

int main() {
    constexpr int ITERATIONS = 100000000;  // 1亿次

    std::cout << "========================================" << std::endl;
    std::cout << "Awaitable 返回方式性能对比" << std::endl;
    std::cout << "迭代次数: " << ITERATIONS << std::endl;
    std::cout << "========================================" << std::endl;

    // 预热
    benchmark<ObjectByValue>("预热", 1000000);

    std::cout << "\n--- 正式测试 ---" << std::endl;

    // 多次测试取平均
    for (int round = 1; round <= 3; ++round) {
        std::cout << "\n第 " << round << " 轮:" << std::endl;
        benchmark<ObjectByValue>("返回对象 (RVO)", ITERATIONS);
        benchmark<ObjectByRef>("返回引用 (带reset)", ITERATIONS);
        benchmark<ObjectByRefNoReset>("返回引用 (无reset)", ITERATIONS);
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "结论:" << std::endl;
    std::cout << "- 如果 '返回对象' 和 '返回引用(无reset)' 接近，说明 RVO 生效" << std::endl;
    std::cout << "- 如果 '返回引用(带reset)' 更慢，说明 reset 有开销" << std::endl;
    std::cout << "========================================" << std::endl;

    return 0;
}
