/**
 * @file bloom_filter.hpp
 * @brief Split-block Bloom Filter
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供高吞吐、非线程安全的 Bloom Filter。实现采用 split-block
 *          布局：每个 block 为 256 bit，由 8 个 uint32_t word 组成；一次
 *          add/query 只访问一个 block，并在每个 word 中设置或检查 1 个 bit。
 */

#ifndef GALAY_UTILS_ALGORITHM_BLOOM_FILTER_HPP
#define GALAY_UTILS_ALGORITHM_BLOOM_FILTER_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace galay::utils {

namespace detail {

struct BloomFilterBlock {
    std::array<uint32_t, 8> words{};
};

inline constexpr std::array<uint32_t, 8> kBloomFilterSalt{
    0x47b6137bU,
    0x44974d91U,
    0x8824ad5bU,
    0xa2b7289dU,
    0x705495c7U,
    0x2df1424bU,
    0x9efc4947U,
    0x5c6bfb31U
};

inline uint64_t mixBloomHash(uint64_t value) noexcept {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    value ^= value >> 31;
    return value;
}

inline size_t nextPowerOfTwo(size_t value) {
    if (value <= 1) {
        return 1;
    }
    if (value > (std::numeric_limits<size_t>::max() >> 1) + 1) {
        throw std::length_error("BloomFilter size is too large");
    }

    --value;
    for (size_t shift = 1; shift < sizeof(size_t) * 8; shift <<= 1) {
        value |= value >> shift;
    }
    return value + 1;
}

} // namespace detail

/**
 * @brief 高吞吐 split-block Bloom Filter
 * @details
 * Bloom Filter 是概率型集合成员判断结构：
 * - add(value) 后，possiblyContains(value) 必定返回 true，除非 clear() 已清空过滤器。
 * - possiblyContains(value) == false 表示 value 一定没有被加入过。
 * - possiblyContains(value) == true 只表示 value 可能被加入过，存在假阳性。
 *
 * 本实现固定每个元素设置 8 个 bit，且 8 个 bit 都落在同一个 256-bit block
 * 的 8 个不同 word 中。相比传统 Bloom Filter 的多次随机位图访问，这种布局
 * 更偏向吞吐和 cache locality；代价是同等 bit 数下假阳性率通常略高。
 *
 * @warning 本类不是精确集合，不能用于权限、安全、资金、风控等必须百分百判断
 *          成员关系的场景。需要精确判断时，请使用真实集合或在返回 true 后回源确认。
 * @warning 本类不支持删除。普通 Bloom Filter 无法安全删除单个元素；需要删除能力
 *          时应使用 Counting Bloom Filter 或其他结构。
 * @warning 本类非线程安全。并发 add/query/clear 同一个实例时必须由调用方外部同步。
 * @warning 默认 Hash 基于进程内 std::hash，不保证跨进程、跨编译器或跨版本稳定。
 *          若要持久化或跨服务共享过滤器，必须提供稳定的 64-bit hash 策略并使用
 *          addHash()/possiblyContainsHash()。
 *
 * @tparam T 待判断的值类型
 * @tparam Hash 返回可转换为 uint64_t 的哈希函数；默认 std::hash<T>
 */
template<typename T, typename Hash = std::hash<T>>
class BloomFilter {
public:
    static constexpr size_t kBitsPerBlock = 256; ///< 每个 split block 的 bit 数
    static constexpr size_t kWordsPerBlock = 8; ///< 每个 block 中的 32-bit word 数
    static constexpr size_t kHashCount = 8; ///< 每次 add/query 设置或检查的 bit 数

    /**
     * @brief 按 bit 数构造 Bloom Filter
     * @param bitCount 期望 bit 数；会向上取整到完整 256-bit block
     * @param hash 哈希函数；结果会经过内部 64-bit 混合后进入 split-block 算法
     * @throws std::invalid_argument bitCount 为 0 时抛出
     * @throws std::length_error block 数超过 uint32_t 可寻址范围时抛出
     */
    explicit BloomFilter(size_t bitCount, Hash hash = Hash{})
        : m_hash(std::move(hash)) {
        if (bitCount == 0) {
            throw std::invalid_argument("BloomFilter bitCount must be greater than 0");
        }

        const size_t blockCount = (bitCount + kBitsPerBlock - 1) / kBitsPerBlock;
        if (blockCount > std::numeric_limits<uint32_t>::max()) {
            throw std::length_error("BloomFilter block count is too large");
        }
        m_blocks.resize(blockCount);
    }

    /**
     * @brief 移动构造 Bloom Filter，转移位图和插入计数
     */
    BloomFilter(BloomFilter&&) noexcept = default;

    /**
     * @brief 移动赋值 Bloom Filter，转移位图和插入计数
     * @return 当前过滤器
     */
    BloomFilter& operator=(BloomFilter&&) noexcept = default;

    /**
     * @brief 显式深拷贝 Bloom Filter 状态
     * @return 独立拥有位图和计数的过滤器副本
     */
    [[nodiscard]] BloomFilter clone() const {
        return BloomFilter(*this);
    }

    /**
     * @brief 根据预计元素数和目标假阳性率构造 Bloom Filter
     * @param expectedItems 预计插入的不同元素数量，必须大于 0
     * @param falsePositiveRate 目标假阳性率，必须位于 (0, 1)
     * @param hash 哈希函数
     * @return 已按 split-block 结构取整后的 BloomFilter
     * @throws std::invalid_argument 参数不合法时抛出
     *
     * @details 这里使用固定 k=8 的近似公式估算 bit 数：
     *          m = -k * n / ln(1 - f^(1/k))。
     *          split-block Bloom Filter 的真实假阳性率还受 block 负载分布影响；
     *          因此该工厂函数是容量建议，不是百分百的误判率承诺。
     */
    static BloomFilter fromExpectedItems(size_t expectedItems,
                                         double falsePositiveRate,
                                         Hash hash = Hash{}) {
        const size_t bits = bitCountForExpectedItems(expectedItems, falsePositiveRate);
        return BloomFilter(bits, std::move(hash));
    }

    /**
     * @brief 估算满足目标假阳性率所需的 bit 数
     * @param expectedItems 预计插入的不同元素数量，必须大于 0
     * @param falsePositiveRate 目标假阳性率，必须位于 (0, 1)
     * @return 向上取整到 256-bit block 的 bit 数
     * @throws std::invalid_argument 参数不合法时抛出
     */
    static size_t bitCountForExpectedItems(size_t expectedItems,
                                           double falsePositiveRate) {
        if (expectedItems == 0) {
            throw std::invalid_argument("BloomFilter expectedItems must be greater than 0");
        }
        if (!(falsePositiveRate > 0.0 && falsePositiveRate < 1.0)) {
            throw std::invalid_argument("BloomFilter falsePositiveRate must be in (0, 1)");
        }

        const double k = static_cast<double>(kHashCount);
        const double n = static_cast<double>(expectedItems);
        const double denominator = std::log(1.0 - std::pow(falsePositiveRate, 1.0 / k));
        const double rawBits = std::ceil((-k * n) / denominator);

        if (!std::isfinite(rawBits) || rawBits <= 0.0 ||
            rawBits > static_cast<double>(std::numeric_limits<size_t>::max() / 2)) {
            throw std::length_error("BloomFilter calculated size is too large");
        }

        const size_t requestedBytes =
            static_cast<size_t>(std::ceil(rawBits / static_cast<double>(8)));
        const size_t roundedBytes =
            std::max<size_t>(kBitsPerBlock / 8, detail::nextPowerOfTwo(requestedBytes));
        return roundedBytes * 8;
    }

    /**
     * @brief 加入一个值
     * @param value 待加入值
     */
    void add(const T& value) {
        addHash(hashValue(value));
    }

    /**
     * @brief 加入一个已计算好的 64-bit hash
     * @param hash64 稳定且分布良好的 64-bit hash
     *
     * @details 该接口不会再次混合 hash，用于调用方已有稳定 hash 的场景。
     */
    void addHash(uint64_t hash64) {
        auto& block = m_blocks[blockIndex(hash64)];
        const auto mask = makeMask(static_cast<uint32_t>(hash64));
        for (size_t i = 0; i < kWordsPerBlock; ++i) {
            block.words[i] |= mask[i];
        }
        ++m_insertions;
    }

    /**
     * @brief 判断一个值是否可能已加入
     * @param value 待判断值
     * @return false 表示一定不存在；true 表示可能存在，需接受假阳性
     */
    bool possiblyContains(const T& value) const {
        return possiblyContainsHash(hashValue(value));
    }

    /**
     * @brief 判断一个 64-bit hash 是否可能已加入
     * @param hash64 稳定且分布良好的 64-bit hash
     * @return false 表示一定不存在；true 表示可能存在，需接受假阳性
     */
    bool possiblyContainsHash(uint64_t hash64) const {
        const auto& block = m_blocks[blockIndex(hash64)];
        const auto mask = makeMask(static_cast<uint32_t>(hash64));
        for (size_t i = 0; i < kWordsPerBlock; ++i) {
            if ((block.words[i] & mask[i]) == 0) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 清空过滤器
     * @details 清空后，之前 add 的元素都会返回 false，直到再次加入。
     */
    void clear() {
        for (auto& block : m_blocks) {
            block.words.fill(0);
        }
        m_insertions = 0;
    }

    size_t bitCount() const noexcept {
        return m_blocks.size() * kBitsPerBlock;
    }

    size_t blockCount() const noexcept {
        return m_blocks.size();
    }

    static constexpr size_t hashCount() noexcept {
        return kHashCount;
    }

    bool empty() const noexcept {
        return m_insertions == 0;
    }

    size_t insertionCount() const noexcept {
        return m_insertions;
    }

private:
    BloomFilter(const BloomFilter&) = default;
    BloomFilter& operator=(const BloomFilter&) = default;

    uint64_t hashValue(const T& value) const {
        using Result = std::invoke_result_t<Hash, const T&>;
        static_assert(std::is_convertible_v<Result, uint64_t>,
                      "BloomFilter Hash result must be convertible to uint64_t");
        return detail::mixBloomHash(static_cast<uint64_t>(m_hash(value)));
    }

    size_t blockIndex(uint64_t hash64) const noexcept {
        const uint64_t high = hash64 >> 32;
        const uint64_t blocks = static_cast<uint64_t>(m_blocks.size());
        return static_cast<size_t>((high * blocks) >> 32);
    }

    static std::array<uint32_t, kWordsPerBlock> makeMask(uint32_t hash32) noexcept {
        std::array<uint32_t, kWordsPerBlock> mask{};
        for (size_t i = 0; i < kWordsPerBlock; ++i) {
            const uint32_t bit =
                (hash32 * detail::kBloomFilterSalt[i]) >> 27;
            mask[i] = uint32_t{1} << bit;
        }
        return mask;
    }

    std::vector<detail::BloomFilterBlock> m_blocks;
    Hash m_hash;
    size_t m_insertions{0};
};

} // namespace galay::utils

#endif // GALAY_UTILS_ALGORITHM_BLOOM_FILTER_HPP
