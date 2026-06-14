/**
 * @file huffman.hpp
 * @brief 哈夫曼编解码实现
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供通用的哈夫曼编码表、编码器、解码器和构建器，
 *          支持从频率表或原始数据构建哈夫曼树。
 */

#ifndef GALAY_UTILS_HUFFMAN_HPP
#define GALAY_UTILS_HUFFMAN_HPP

#include "utils/common/defn.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <queue>
#include <memory>
#include <stdexcept>

namespace galay::utils {

/// 哈夫曼编码结构体
struct HuffmanCode {
    uint32_t code; ///< 编码值
    uint8_t length; ///< 编码位长

    HuffmanCode() : code(0), length(0) {}
    HuffmanCode(uint32_t c, uint8_t len) : code(c), length(len) {}
};

/**
 * @brief 哈夫曼树节点
 * @tparam T 符号类型
 */
template<typename T>
struct HuffmanNode {
    T symbol; ///< 符号值
    size_t frequency; ///< 频率
    std::unique_ptr<HuffmanNode> left; ///< 左子节点
    std::unique_ptr<HuffmanNode> right; ///< 右子节点

    HuffmanNode(T sym, size_t freq)
        : symbol(sym), frequency(freq) {}

    HuffmanNode(std::unique_ptr<HuffmanNode> l, std::unique_ptr<HuffmanNode> r)
        : symbol(), frequency(l->frequency + r->frequency)
        , left(std::move(l)), right(std::move(r)) {}

    bool isLeaf() const { return !left && !right; } ///< 判断是否为叶子节点
};

/**
 * @brief 哈夫曼编码表
 * @details 维护符号到编码的正向和反向映射。
 * @tparam T 符号类型
 */
template<typename T>
class HuffmanTable {
public:
    HuffmanTable() = default;

    /**
     * @brief 添加符号编码映射
     * @param symbol 符号
     * @param code 编码值
     * @param length 编码位长
     */
    void addCode(const T& symbol, uint32_t code, uint8_t length) {
        if (length > 32) {
            throw std::invalid_argument("Code length exceeds 32 bits");
        }
        m_encodeTable[symbol] = HuffmanCode(code, length);
        m_decodeTable[{code, length}] = symbol;
    }

    /**
     * @brief 获取符号的哈夫曼编码
     * @param symbol 目标符号
     * @return 哈夫曼编码引用
     */
    const HuffmanCode& getCode(const T& symbol) const {
        auto it = m_encodeTable.find(symbol);
        if (it == m_encodeTable.end()) {
            throw std::runtime_error("Symbol not found in table");
        }
        return it->second;
    }

    /**
     * @brief 检查符号是否存在
     * @param symbol 目标符号
     * @return 存在返回 true
     */
    bool hasSymbol(const T& symbol) const {
        return m_encodeTable.find(symbol) != m_encodeTable.end();
    }

    /**
     * @brief 根据编码获取符号
     * @param code 编码值
     * @param length 编码位长
     * @return 对应的符号引用
     */
    const T& getSymbol(uint32_t code, uint8_t length) const {
        auto it = m_decodeTable.find({code, length});
        if (it == m_decodeTable.end()) {
            throw std::runtime_error("Code not found in table");
        }
        return it->second;
    }

    /**
     * @brief 尝试根据编码获取符号
     * @param code 编码值
     * @param length 编码位长
     * @param symbol 输出符号
     * @return 成功返回 true
     */
    bool tryGetSymbol(uint32_t code, uint8_t length, T& symbol) const {
        auto it = m_decodeTable.find({code, length});
        if (it != m_decodeTable.end()) {
            symbol = it->second;
            return true;
        }
        return false;
    }

    /**
     * @brief 获取所有符号列表
     * @return 符号向量
     */
    std::vector<T> getSymbols() const {
        std::vector<T> result;
        result.reserve(m_encodeTable.size());
        for (const auto& [sym, code] : m_encodeTable) {
            result.push_back(sym);
        }
        return result;
    }

    /**
     * @brief 获取编码表大小
     * @return 符号数量
     */
    size_t size() const { return m_encodeTable.size(); }

    /**
     * @brief 清空编码表
     */
    void clear() {
        m_encodeTable.clear();
        m_decodeTable.clear();
    }

private:
    struct CodeKey {
        uint32_t code;
        uint8_t length;

        bool operator==(const CodeKey& other) const {
            return code == other.code && length == other.length;
        }
    };

    struct CodeKeyHash {
        size_t operator()(const CodeKey& key) const {
            return std::hash<uint32_t>()(key.code) ^
                   (std::hash<uint8_t>()(key.length) << 1);
        }
    };

    std::unordered_map<T, HuffmanCode> m_encodeTable;
    std::unordered_map<CodeKey, T, CodeKeyHash> m_decodeTable;
};

/**
 * @brief 哈夫曼编码器
 * @details 将符号按哈夫曼编码表逐位编码为字节流。
 * @tparam T 符号类型
 */
template<typename T>
class HuffmanEncoder {
public:
    /**
     * @brief 构造编码器
     * @param table 哈夫曼编码表引用
     */
    explicit HuffmanEncoder(const HuffmanTable<T>& table)
        : m_table(table), m_buffer(0), m_bufferBits(0) {}

    /**
     * @brief 编码单个符号
     * @param symbol 待编码的符号
     */
    void encode(const T& symbol) {
        const auto& code = m_table.getCode(symbol);
        appendBits(code.code, code.length);
    }

    /**
     * @brief 编码符号序列
     * @param symbols 待编码的符号向量
     */
    void encode(const std::vector<T>& symbols) {
        for (const auto& sym : symbols) {
            encode(sym);
        }
    }

    /**
     * @brief 完成编码并返回字节流
     * @return 编码后的字节数组
     */
    std::vector<uint8_t> finish() {
        if (m_bufferBits > 0) {
            m_output.push_back(static_cast<uint8_t>(m_buffer << (8 - m_bufferBits)));
        }
        auto result = std::move(m_output);
        reset();
        return result;
    }

    /**
     * @brief 获取当前编码位总数
     * @return 位数量
     */
    size_t bitCount() const {
        return m_output.size() * 8 + m_bufferBits;
    }

    /**
     * @brief 重置编码器状态
     */
    void reset() {
        m_output.clear();
        m_buffer = 0;
        m_bufferBits = 0;
    }

private:
    void appendBits(uint32_t bits, uint8_t count) {
        while (count > 0) {
            uint8_t available = 8 - m_bufferBits;
            uint8_t toWrite = std::min(available, count);

            uint8_t shift = count - toWrite;
            uint8_t extracted = (bits >> shift) & ((1 << toWrite) - 1);

            m_buffer = (m_buffer << toWrite) | extracted;
            m_bufferBits += toWrite;
            count -= toWrite;

            if (m_bufferBits == 8) {
                m_output.push_back(static_cast<uint8_t>(m_buffer));
                m_buffer = 0;
                m_bufferBits = 0;
            }
        }
    }

    const HuffmanTable<T>& m_table;
    std::vector<uint8_t> m_output;
    uint32_t m_buffer;
    uint8_t m_bufferBits;
};

/**
 * @brief 哈夫曼解码器
 * @details 逐位解码字节流为符号序列。
 * @tparam T 符号类型
 */
template<typename T>
class HuffmanDecoder {
public:
    /**
     * @brief 构造解码器
     * @param table 哈夫曼编码表引用
     * @param minCodeLen 最小编码长度（默认 1）
     * @param maxCodeLen 最大编码长度（默认 32）
     */
    HuffmanDecoder(const HuffmanTable<T>& table, uint8_t minCodeLen = 1, uint8_t maxCodeLen = 32)
        : m_table(table), m_minCodeLen(minCodeLen), m_maxCodeLen(maxCodeLen) {}

    /**
     * @brief 解码字节流
     * @param data 待解码的字节数组
     * @param symbolCount 期望解码的符号数（0 表示全部解码）
     * @return 解码后的符号向量
     */
    std::vector<T> decode(const std::vector<uint8_t>& data, size_t symbolCount = 0) {
        std::vector<T> result;
        uint32_t code = 0;
        uint8_t codeLen = 0;
        size_t bitIndex = 0;

        while (bitIndex < data.size() * 8) {
            size_t byteIndex = bitIndex / 8;
            size_t bitOffset = 7 - (bitIndex % 8);
            uint8_t bit = (data[byteIndex] >> bitOffset) & 1;

            code = (code << 1) | bit;
            ++codeLen;
            ++bitIndex;

            if (codeLen >= m_minCodeLen) {
                T symbol;
                if (m_table.tryGetSymbol(code, codeLen, symbol)) {
                    result.push_back(symbol);
                    code = 0;
                    codeLen = 0;

                    if (symbolCount > 0 && result.size() >= symbolCount) {
                        break;
                    }
                }
            }

            if (codeLen > m_maxCodeLen) {
                throw std::runtime_error("Invalid Huffman code");
            }
        }

        return result;
    }

private:
    const HuffmanTable<T>& m_table;
    uint8_t m_minCodeLen;
    uint8_t m_maxCodeLen;
};

/**
 * @brief 哈夫曼树构建器
 * @details 从频率表或原始数据构建哈夫曼编码表。
 * @tparam T 符号类型
 */
template<typename T>
class HuffmanBuilder {
public:
    /**
     * @brief 根据频率表构建哈夫曼编码表
     * @param frequencies 符号到频率的映射
     * @return 构建好的哈夫曼编码表
     */
    static HuffmanTable<T> build(const std::unordered_map<T, size_t>& frequencies) {
        if (frequencies.empty()) {
            return HuffmanTable<T>();
        }

        auto cmp = [](const std::unique_ptr<HuffmanNode<T>>& a,
                      const std::unique_ptr<HuffmanNode<T>>& b) {
            return a->frequency > b->frequency;
        };

        std::priority_queue<std::unique_ptr<HuffmanNode<T>>,
                           std::vector<std::unique_ptr<HuffmanNode<T>>>,
                           decltype(cmp)> pq(cmp);

        for (const auto& [symbol, freq] : frequencies) {
            pq.push(std::make_unique<HuffmanNode<T>>(symbol, freq));
        }

        while (pq.size() > 1) {
            auto left = std::move(const_cast<std::unique_ptr<HuffmanNode<T>>&>(pq.top()));
            pq.pop();
            auto right = std::move(const_cast<std::unique_ptr<HuffmanNode<T>>&>(pq.top()));
            pq.pop();

            pq.push(std::make_unique<HuffmanNode<T>>(std::move(left), std::move(right)));
        }

        HuffmanTable<T> table;
        if (!pq.empty()) {
            generateCodes(pq.top().get(), 0, 0, table);
        }

        return table;
    }

    /**
     * @brief 根据原始数据构建哈夫曼编码表
     * @param data 原始符号数据
     * @return 构建好的哈夫曼编码表
     */
    static HuffmanTable<T> buildFromData(const std::vector<T>& data) {
        std::unordered_map<T, size_t> frequencies;
        for (const auto& item : data) {
            ++frequencies[item];
        }
        return build(frequencies);
    }

private:
    static void generateCodes(HuffmanNode<T>* node, uint32_t code, uint8_t length,
                              HuffmanTable<T>& table) {
        if (!node) return;

        if (node->isLeaf()) {
            if (length == 0) {
                table.addCode(node->symbol, 0, 1);
            } else {
                table.addCode(node->symbol, code, length);
            }
            return;
        }

        generateCodes(node->left.get(), (code << 1), length + 1, table);
        generateCodes(node->right.get(), (code << 1) | 1, length + 1, table);
    }
};

} // namespace galay::utils

#endif // GALAY_UTILS_HUFFMAN_HPP
