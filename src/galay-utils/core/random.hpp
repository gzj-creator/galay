/**
 * @file random.hpp
 * @brief 随机数生成器
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供本地无锁随机数生成器和线程安全单例随机数生成器，支持整数、
 *          浮点数、布尔值、字符串、十六进制、字节和 UUID 生成。
 */

#ifndef GALAY_UTILS_RANDOM_HPP
#define GALAY_UTILS_RANDOM_HPP

#include "galay-utils/common/defn.hpp"
#include <random>
#include <string>
#include <string_view>
#include <mutex>

namespace galay::utils {

/**
 * @brief 本地随机数生成器
 * @details 使用 Mersenne Twister 19937_64 引擎。不包含互斥锁，非线程安全；
 *          多线程或多个协程并发共享同一个实例时必须由调用方外部同步。
 *          推荐在协程热路径或单线程上下文中使用独立实例，避免共享单例锁竞争。
 */
class RandomGenerator {
public:
    /**
     * @brief 使用随机设备构造生成器
     */
    RandomGenerator() {
        reseed();
    }

    /**
     * @brief 使用固定种子构造生成器
     * @param seedValue 种子值；相同种子会生成相同序列
     */
    explicit RandomGenerator(uint64_t seedValue)
        : m_engine(seedValue) {}

    /**
     * @brief 生成随机整数
     * @param min 最小值（含）
     * @param max 最大值（含）
     * @return min < max 时返回闭区间随机数；否则返回 min
     */
    int randomInt(int min, int max) {
        if (min >= max) return min;
        std::uniform_int_distribution<int> dist(min, max);
        return dist(m_engine);
    }

    /**
     * @brief 生成随机 32 位无符号整数
     * @param min 最小值（含）
     * @param max 最大值（含）
     * @return min < max 时返回闭区间随机数；否则返回 min
     */
    uint32_t randomUint32(uint32_t min, uint32_t max) {
        if (min >= max) return min;
        std::uniform_int_distribution<uint32_t> dist(min, max);
        return dist(m_engine);
    }

    /**
     * @brief 生成随机 64 位无符号整数
     * @param min 最小值（含）
     * @param max 最大值（含）
     * @return min < max 时返回闭区间随机数；否则返回 min
     */
    uint64_t randomUint64(uint64_t min, uint64_t max) {
        if (min >= max) return min;
        std::uniform_int_distribution<uint64_t> dist(min, max);
        return dist(m_engine);
    }

    /**
     * @brief 生成随机双精度浮点数
     * @param min 最小值（含）
     * @param max 最大值（不含）
     * @return min < max 时返回半开区间随机数；否则返回 min
     */
    double randomDouble(double min, double max) {
        if (min >= max) return min;
        std::uniform_real_distribution<double> dist(min, max);
        return dist(m_engine);
    }

    /**
     * @brief 生成随机单精度浮点数
     * @param min 最小值（含）
     * @param max 最大值（不含）
     * @return min < max 时返回半开区间随机数；否则返回 min
     */
    float randomFloat(float min, float max) {
        if (min >= max) return min;
        std::uniform_real_distribution<float> dist(min, max);
        return dist(m_engine);
    }

    /**
     * @brief 生成随机布尔值
     * @param probability 为 true 的概率；小于等于 0 返回 false，大于等于 1 返回 true
     * @return 随机布尔值
     */
    bool randomBool(double probability = 0.5) {
        if (probability <= 0.0) return false;
        if (probability >= 1.0) return true;
        std::bernoulli_distribution dist(probability);
        return dist(m_engine);
    }

    /**
     * @brief 生成随机字符串
     * @param length 字符串长度
     * @param charset 字符集（默认为字母数字）；为空时返回空字符串
     * @return 随机字符串；length 为 0 或 charset 为空时返回空字符串
     */
    std::string randomString(size_t length,
                             std::string_view charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") {
        if (charset.empty() || length == 0) return "";

        std::string result;
        result.resize(length); // 预分配内存

        std::uniform_int_distribution<size_t> dist(0, charset.length() - 1);

        for (size_t i = 0; i < length; ++i) {
            result[i] = charset[dist(m_engine)]; // 直接赋值，避免+=操作
        }

        return result;
    }

    /**
     * @brief 生成随机十六进制字符串
     * @param length 字符串长度
     * @param uppercase 是否使用大写字母
     * @return 随机十六进制字符串；length 为 0 时返回空字符串
     */
    std::string randomHex(size_t length, bool uppercase = false) {
        if (length == 0) return "";

        const char* hexChars = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

        std::string result;
        result.resize(length); // 预分配内存

        std::uniform_int_distribution<int> dist(0, 15);

        for (size_t i = 0; i < length; ++i) {
            result[i] = hexChars[dist(m_engine)]; // 直接赋值
        }

        return result;
    }

    /**
     * @brief 生成随机字节序列
     * @param buffer 输出缓冲区；为空时直接返回
     * @param length 字节数量
     */
    void randomBytes(uint8_t* buffer, size_t length) {
        if (buffer == nullptr || length == 0) return;

        std::uniform_int_distribution<int> dist(0, 255);

        for (size_t i = 0; i < length; ++i) {
            buffer[i] = static_cast<uint8_t>(dist(m_engine));
        }
    }

    /**
     * @brief 生成 UUID（版本 4）
     * @return 36 字符的 UUID 字符串
     */
    std::string uuid() {
        std::uniform_int_distribution<int> dist(0, 15);
        std::uniform_int_distribution<int> dist2(8, 11);

        const char* hexChars = "0123456789abcdef";
        std::string uuid(36, '-');

        for (int i = 0; i < 36; ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                continue;
            } else if (i == 14) {
                uuid[i] = '4';
            } else if (i == 19) {
                uuid[i] = hexChars[dist2(m_engine)];
            } else {
                uuid[i] = hexChars[dist(m_engine)];
            }
        }

        return uuid;
    }

    /**
     * @brief 使用固定种子重置生成器状态
     * @param seedValue 种子值；相同种子会生成相同序列
     */
    void seed(uint64_t seed) {
        m_engine.seed(seed);
    }

    /**
     * @brief 使用随机设备重新播种
     */
    void reseed() {
        std::random_device rd;
        m_engine.seed(rd());
    }

private:
    std::mt19937_64 m_engine;
};

/**
 * @brief 线程安全随机数生成器单例
 * @details 单例模式，内部持有 RandomGenerator 并使用互斥锁保护共享状态。
 *          所有需要推进随机引擎状态的方法都会短暂加锁，因此可安全跨线程共享，
 *          但不适合协程调度线程或高频热路径；这些场景应优先使用调用方自持有、
 *          自同步的 RandomGenerator。
 */
class Randomizer {
public:
    /**
     * @brief 获取单例实例
     * @return 随机数生成器实例引用
     */
    static Randomizer& instance() {
        static Randomizer instance;
        return instance;
    }

    /**
     * @brief 生成随机整数
     * @param min 最小值（含）
     * @param max 最大值（含）
     * @return min < max 时返回闭区间随机数；否则返回 min
     */
    int randomInt(int min, int max) {
        if (min >= max) return min;
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_generator.randomInt(min, max);
    }

    /**
     * @brief 生成随机 32 位无符号整数
     * @param min 最小值（含）
     * @param max 最大值（含）
     * @return min < max 时返回闭区间随机数；否则返回 min
     */
    uint32_t randomUint32(uint32_t min, uint32_t max) {
        if (min >= max) return min;
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_generator.randomUint32(min, max);
    }

    /**
     * @brief 生成随机 64 位无符号整数
     * @param min 最小值（含）
     * @param max 最大值（含）
     * @return min < max 时返回闭区间随机数；否则返回 min
     */
    uint64_t randomUint64(uint64_t min, uint64_t max) {
        if (min >= max) return min;
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_generator.randomUint64(min, max);
    }

    /**
     * @brief 生成随机双精度浮点数
     * @param min 最小值（含）
     * @param max 最大值（不含）
     * @return min < max 时返回半开区间随机数；否则返回 min
     */
    double randomDouble(double min, double max) {
        if (min >= max) return min;
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_generator.randomDouble(min, max);
    }

    /**
     * @brief 生成随机单精度浮点数
     * @param min 最小值（含）
     * @param max 最大值（不含）
     * @return min < max 时返回半开区间随机数；否则返回 min
     */
    float randomFloat(float min, float max) {
        if (min >= max) return min;
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_generator.randomFloat(min, max);
    }

    /**
     * @brief 生成随机布尔值
     * @param probability 为 true 的概率；小于等于 0 返回 false，大于等于 1 返回 true
     * @return 随机布尔值
     */
    bool randomBool(double probability = 0.5) {
        if (probability <= 0.0) return false;
        if (probability >= 1.0) return true;
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_generator.randomBool(probability);
    }

    /**
     * @brief 生成随机字符串
     * @param length 字符串长度
     * @param charset 字符集（默认为字母数字）；为空时返回空字符串
     * @return 随机字符串；length 为 0 或 charset 为空时返回空字符串
     */
    std::string randomString(size_t length,
                             std::string_view charset = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") {
        if (charset.empty() || length == 0) return "";
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_generator.randomString(length, charset);
    }

    /**
     * @brief 生成随机十六进制字符串
     * @param length 字符串长度
     * @param uppercase 是否使用大写字母
     * @return 随机十六进制字符串；length 为 0 时返回空字符串
     */
    std::string randomHex(size_t length, bool uppercase = false) {
        if (length == 0) return "";
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_generator.randomHex(length, uppercase);
    }

    /**
     * @brief 生成随机字节序列
     * @param buffer 输出缓冲区；为空时直接返回
     * @param length 字节数量
     */
    void randomBytes(uint8_t* buffer, size_t length) {
        if (buffer == nullptr || length == 0) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_generator.randomBytes(buffer, length);
    }

    /**
     * @brief 生成 UUID（版本 4）
     * @return 36 字符的 UUID 字符串
     */
    std::string uuid() {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_generator.uuid();
    }

    /**
     * @brief 使用固定种子重置生成器状态
     * @param seedValue 种子值；相同种子会生成相同序列
     */
    void seed(uint64_t seed) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_generator.seed(seed);
    }

    /**
     * @brief 使用随机设备重新播种
     */
    void reseed() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_generator.reseed();
    }

private:
    Randomizer() = default;
    ~Randomizer() = default;

    Randomizer(const Randomizer&) = delete;
    Randomizer& operator=(const Randomizer&) = delete;

    RandomGenerator m_generator;
    std::mutex m_mutex;
};

} // namespace galay::utils

#endif // GALAY_UTILS_RANDOM_HPP
