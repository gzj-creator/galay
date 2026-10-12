#ifndef GALAY_UTILS_SYSTEM_RDTSC_HPP
#define GALAY_UTILS_SYSTEM_RDTSC_HPP

#include <chrono>
#include <cstdint>
#include <expected>
#include <limits>
#include <system_error>
#include <utility>

namespace galay::utils {

/**
 * @brief 硬件计数器频率的获取来源。
 */
enum class RdtscFrequencySource {
    /** @brief 频率来自 x86-64 CPUID 或 AArch64 CNTFRQ_EL0 提供的架构信息。 */
    kArchitectural,

    /** @brief 架构频率信息不完整时，通过参考时钟校准得到频率。 */
    kCalibrated
};

/**
 * @brief 显式初始化的硬件计数器，用于固定同一 CPU 的短区间计时。
 * @details x86-64 读取 TSC，AArch64 读取固定频率的系统计时器；tick 不等于
 *          当前 CPU 核心周期，也没有 Unix 或 steady_clock 的时间纪元。
 *          应在启动工作线程或协程前完成初始化，x86 校准可能阻塞约 100 ms。
 *          测量线程必须固定到同一 CPU；本类型不保证跨 CPU 同步，也不保证
 *          休眠恢复、虚拟机迁移或计数频率变化后的计时有效性。
 */
class Rdtsc {
public:
    /**
     * @brief 编译器、架构与 128 位整数是否支持此实现。
     * @note 此标志不代表运行时硬件能力和访问权限已满足，仍需检查 create() 的结果。
     */
#if (defined(__GNUC__) || defined(__clang__)) && \
    defined(__SIZEOF_INT128__) && (defined(__x86_64__) || defined(__aarch64__))
    static constexpr bool is_supported = true;
#else
    static constexpr bool is_supported = false;
#endif

    /**
     * @brief 在初始化阶段获取计数频率并预计算换算参数。
     * @return 初始化成功的计数器；失败时返回不支持、访问权限、校准或数值范围错误。
     * @note Linux x86 只检查初始化线程的 TSC 权限，各读取线程必须保持访问权限。
     *       AArch64 调用方须事先确认系统允许用户态读取 CNTVCT_EL0 和 CNTFRQ_EL0；
     *       本函数不捕获寄存器访问陷阱，也不会在不支持时改用其他时钟。
     */
    [[nodiscard]] static std::expected<Rdtsc, std::error_code> create();

    /**
     * @brief 以最低开销读取原始 tick，不约束周围指令的执行顺序。
     * @note 不适合直接用作要求严格指令排序的区间测量边界。
     */
    [[nodiscard]] inline std::uint64_t read_relaxed() const noexcept {
#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
        std::uint32_t low;
        std::uint32_t high;
        __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
        return (static_cast<std::uint64_t>(high) << 32U) | low;
#elif (defined(__GNUC__) || defined(__clang__)) && defined(__aarch64__)
        std::uint64_t value;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(value));
        return value;
#else
        std::unreachable(); // 不支持的平台无法通过 create() 构造有效对象。
#endif
    }

    /**
     * @brief 读取计时起点，在读取两侧设置架构指令屏障。
     * @note x86 的 LFENCE 必须具备执行序列化语义，旧 AMD CPU 上依赖系统配置。
     *       它等待前序指令和本地加载完成，不保证存储已对其他 CPU 可见。
     *       AArch64 的 ISB 同步指令执行，不保证内存访问完成。
     *       memory 约束仅限制编译器重排内存操作，不能阻止无关纯算术表达式移动。
     */
    [[nodiscard]] inline std::uint64_t read_start() const noexcept {
#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
        std::uint32_t low;
        std::uint32_t high;
        __asm__ volatile("lfence\n\trdtsc\n\tlfence"
                         : "=a"(low), "=d"(high) : : "memory");
        return (static_cast<std::uint64_t>(high) << 32U) | low;
#elif (defined(__GNUC__) || defined(__clang__)) && defined(__aarch64__)
        std::uint64_t value;
        __asm__ volatile("isb\n\tmrs %0, cntvct_el0\n\tisb"
                         : "=r"(value) : : "memory");
        return value;
#else
        std::unreachable();
#endif
    }

    /**
     * @brief 读取计时终点，x86 使用 RDTSCP 后接 LFENCE。
     * @note x86 等待前序指令和本地加载完成，不保证存储已对其他 CPU 可见。
     *       AArch64 与 read_start() 使用相同屏障；编译器排序限制同 read_start()。
     */
    [[nodiscard]] inline std::uint64_t read_end() const noexcept {
#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
        std::uint32_t low;
        std::uint32_t high;
        std::uint32_t auxiliary;
        __asm__ volatile("rdtscp\n\tlfence"
                         : "=a"(low), "=d"(high), "=c"(auxiliary) : : "memory");
        return (static_cast<std::uint64_t>(high) << 32U) | low;
#elif (defined(__GNUC__) || defined(__clang__)) && defined(__aarch64__)
        return read_start();
#else
        std::unreachable();
#endif
    }

    /**
     * @brief 使用预计算的 Q32 定点乘数，将 tick 差值换算为纳秒。
     * @param ticks 起止读数之差，不是原始计数器时间戳。
     * @return 向下取整的近似纳秒数；结果超出有符号 64 位范围时返回 value_too_large。
     * @note 相对给定频率的精确换算，Q32 截断少算的误差小于 ticks / 2^32 + 1 ns，
     *       频率估计误差需另行计算。调用方须在相减前检查计数器回绕或读数回退。
     */
    [[nodiscard]] inline std::expected<std::chrono::nanoseconds, std::error_code>
    to_nanoseconds(std::uint64_t ticks) const noexcept {
#if defined(__SIZEOF_INT128__)
        using Wide = unsigned __int128;
        const Wide scaled = (static_cast<Wide>(ticks) * m_ns_multiplier) >> 32U;
        if (scaled > static_cast<Wide>(std::numeric_limits<std::int64_t>::max())) {
            return std::unexpected(std::make_error_code(std::errc::value_too_large));
        }
        return std::chrono::nanoseconds{static_cast<std::int64_t>(scaled)};
#else
        std::unreachable();
#endif
    }

    /**
     * @brief 获取初始化时确定的计数频率。
     * @return 每秒 tick 数（Hz），不是当前 CPU 核心主频。
     */
    [[nodiscard]] std::uint64_t frequency_hz() const noexcept { return m_frequency_hz; }

    /**
     * @brief 获取计数频率的来源。
     * @return 架构信息或参考时钟校准，含义见 RdtscFrequencySource。
     */
    [[nodiscard]] RdtscFrequencySource frequency_source() const noexcept { return m_source; }

    /**
     * @brief 查询架构是否声明计数频率稳定。
     * @return x86 返回 CPUID 的 invariant TSC 标记，AArch64 固定频率计时器返回 true。
     * @note 此标志不保证跨 CPU 同步，也不保证休眠恢复或虚拟机迁移后的连续性。
     */
    [[nodiscard]] bool has_invariant_frequency() const noexcept { return m_invariant; }

private:
    Rdtsc(std::uint64_t frequency_hz, std::uint64_t multiplier,
          RdtscFrequencySource source, bool invariant) noexcept
        : m_frequency_hz(frequency_hz), m_ns_multiplier(multiplier),
          m_source(source), m_invariant(invariant) {}

    std::uint64_t m_frequency_hz;
    std::uint64_t m_ns_multiplier; ///< 纳秒/tick 的 Q32 定点乘数，避免热路径执行除法。
    RdtscFrequencySource m_source;
    bool m_invariant;
};

} // namespace galay::utils

#endif // GALAY_UTILS_SYSTEM_RDTSC_HPP
