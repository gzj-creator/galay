/**
 * @file trie.hpp
 * @brief 字典树（Trie）实现
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供字典树的插入、查找、前缀匹配、删除和词频统计功能。
 */

#ifndef GALAY_UTILS_TRIE_TREE_HPP
#define GALAY_UTILS_TRIE_TREE_HPP

#include "utils/common/defn.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>

namespace galay::utils {

/**
 * @brief 字典树节点
 * @details 存储子节点映射、单词结束标记和词频计数。
 */
class TrieNode {
public:
    TrieNode() : m_isEnd(false), m_count(0) {}

    std::unordered_map<char, std::unique_ptr<TrieNode>> children; ///< 子节点映射
    bool m_isEnd; ///< 是否为单词结束节点
    int m_count; ///< 经过此节点的单词计数
};

/**
 * @brief 字典树（Trie）
 * @details 支持单词插入、查找、前缀匹配、删除和词频统计。
 */
class TrieTree {
public:
    TrieTree() : m_root(std::make_unique<TrieNode>()), m_size(0) {}

    /**
     * @brief 添加单词到字典树
     * @param word 待添加的单词
     */
    void add(const std::string& word) {
        if (word.empty()) return;

        TrieNode* node = m_root.get();
        for (char c : word) {
            if (node->children.find(c) == node->children.end()) {
                node->children[c] = std::make_unique<TrieNode>();
            }
            node = node->children[c].get();
        }

        if (!node->m_isEnd) {
            node->m_isEnd = true;
            ++m_size;
        }
        ++node->m_count;
    }

    /**
     * @brief 检查字典树是否包含指定单词
     * @param word 目标单词
     * @return 存在返回 true
     */
    bool contains(const std::string& word) const {
        TrieNode* node = findNode(word);
        return node != nullptr && node->m_isEnd;
    }

    /**
     * @brief 检查是否存在以指定前缀开头的单词
     * @param prefix 前缀字符串
     * @return 存在返回 true
     */
    bool startsWith(const std::string& prefix) const {
        return findNode(prefix) != nullptr;
    }

    /**
     * @brief 查询单词的出现次数
     * @param word 目标单词
     * @return 出现次数，不存在返回 0
     */
    int query(const std::string& word) const {
        TrieNode* node = findNode(word);
        if (node && node->m_isEnd) {
            return node->m_count;
        }
        return 0;
    }

    /**
     * @brief 从字典树中移除单词
     * @param word 目标单词
     * @return 移除成功返回 true
     */
    bool remove(const std::string& word) {
        if (word.empty() || !contains(word)) {
            return false;
        }

        removeHelper(m_root.get(), word, 0);
        --m_size;
        return true;
    }

    /**
     * @brief 获取以指定前缀开头的所有单词
     * @param prefix 前缀字符串
     * @return 匹配的单词列表
     */
    std::vector<std::string> getWordsWithPrefix(const std::string& prefix) const {
        std::vector<std::string> result;
        TrieNode* node = findNode(prefix);

        if (node) {
            collectWords(node, prefix, result);
        }

        return result;
    }

    /**
     * @brief 获取字典树中的所有单词
     * @return 所有单词列表
     */
    std::vector<std::string> getAllWords() const {
        std::vector<std::string> result;
        collectWords(m_root.get(), "", result);
        return result;
    }

    size_t size() const { return m_size; } ///< 获取单词数量
    bool empty() const { return m_size == 0; } ///< 判断字典树是否为空

    void clear() {
        m_root = std::make_unique<TrieNode>();
        m_size = 0;
    }

private:
    TrieNode* findNode(const std::string& prefix) const {
        TrieNode* node = m_root.get();
        for (char c : prefix) {
            auto it = node->children.find(c);
            if (it == node->children.end()) {
                return nullptr;
            }
            node = it->second.get();
        }
        return node;
    }

    void collectWords(TrieNode* node, const std::string& prefix, std::vector<std::string>& result) const {
        if (node->m_isEnd) {
            result.push_back(prefix);
        }

        for (const auto& [c, child] : node->children) {
            collectWords(child.get(), prefix + c, result);
        }
    }

    bool removeHelper(TrieNode* node, const std::string& word, size_t depth) {
        if (depth == word.length()) {
            if (node->m_isEnd) {
                node->m_isEnd = false;
                node->m_count = 0;
                return node->children.empty();
            }
            return false;
        }

        char c = word[depth];
        auto it = node->children.find(c);
        if (it == node->children.end()) {
            return false;
        }

        bool shouldDelete = removeHelper(it->second.get(), word, depth + 1);

        if (shouldDelete) {
            node->children.erase(it);
            return !node->m_isEnd && node->children.empty();
        }

        return false;
    }

    std::unique_ptr<TrieNode> m_root;
    size_t m_size;
};

} // namespace galay::utils

#endif // GALAY_UTILS_TRIE_TREE_HPP
