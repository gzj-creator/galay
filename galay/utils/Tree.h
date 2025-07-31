#ifndef __GALAY_TREE_H__
#define __GALAY_TREE_H__

#include <stdint.h>
#include <map>
#include <vector>
#include <queue>
#include <string>

namespace galay::utils
{
    class TrieTree
    {
    private:
        struct TrieNode
        {
            TrieNode(char value, uint32_t frequence = 0);
            char m_value;
            uint32_t m_frequence;
            std::map<char, TrieNode*> m_childMap;
        };
    public:
        TrieTree();
        virtual ~TrieTree();
        void add(const std::string& word);
        bool contains(const std::string& word);
        uint32_t query(const std::string& word);
        void remove(const std::string& word);
        std::vector<std::string> getWordByPrefix(const std::string& prefix);
    private:
        void preOrder(TrieNode* node, std::string word, std::vector<std::string>& words);
    private:
        TrieNode* m_root;
    };
}

#endif