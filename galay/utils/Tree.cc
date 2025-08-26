#include "Tree.h"

namespace galay::utils 
{
TrieTree::TrieNode::TrieNode(char value, uint32_t frequence)
{
    this->m_value = value;
    this->m_frequence = frequence;
}

TrieTree::TrieTree()
{
    m_root = new TrieNode('\0', 0);
}

void 
TrieTree::add(const std::string& word)
{
    TrieNode* cur = m_root;
    for (auto& c : word)
    {
        auto childIt = cur->m_childMap.find(c);
        if (childIt == cur->m_childMap.end())
        {
            TrieNode* child = new TrieNode(c);
            cur->m_childMap.emplace(c, child);
            cur = child;
        }
        else
        {
            cur = childIt->second;
        }
    }
    ++cur->m_frequence;
}

bool 
TrieTree::contains(const std::string& word)
{
    return query(word) != 0;
}

uint32_t 
TrieTree::query(const std::string& word)
{
    TrieNode* cur = m_root;
    for (auto& c : word)
    {
        auto childIt = cur->m_childMap.find(c);
        if (childIt == cur->m_childMap.end())
        {
            return 0;
        }
        else
        {
            cur = childIt->second;
        }
    }
    return cur->m_frequence;
}

void 
TrieTree::remove(const std::string& word)
{
    TrieNode* cur = m_root;
    TrieNode* del = m_root;
    char delCh = word[0];
    for (size_t i = 0; i < word.length() ; ++i)
    {
        auto childIt = cur->m_childMap.find(word[i]);
        if (childIt == cur->m_childMap.end())
        {
            return;
        }
        else
        {
            if(cur->m_frequence > 0 || cur->m_childMap.size() > 1)
            {
                del = cur;
                delCh = word[i];
            }
            cur = childIt->second;
        }

        if(cur->m_childMap.empty())
        {
            TrieNode* child = del->m_childMap[delCh];
            del->m_childMap.erase(delCh);

            std::queue<TrieNode*> q;
            q.push(child);
            while(!q.empty())
            {
                TrieNode* node = q.front();
                q.pop();
                for( auto pair : node->m_childMap)
                {
                    q.push(pair.second);
                }
                delete node;
            }
        }
        else
        {
            cur->m_frequence = 0;
        }
    }
    
}

std::vector<std::string> 
TrieTree::getWordByPrefix(const std::string& prefix)
{
    TrieNode* cur = m_root;
    for (auto& c : prefix)
    {
        auto childIt = cur->m_childMap.find(c);
        if (childIt == cur->m_childMap.end())
        {
            return {};
        }
        else
        {
            cur = childIt->second;
        }
    }
    std::vector<std::string> words;
    preOrder(cur, prefix.substr(0, prefix.length() - 1), words);
    return words;
}

void 
TrieTree::preOrder(TrieNode* node, std::string word, std::vector<std::string>& words)
{
    if( node != this->m_root )
    {
        word.push_back(node->m_value);
        if(node->m_frequence > 0)
        {
            words.push_back(word);
        }
    }

    for( auto pair : node->m_childMap)
    {
        preOrder(pair.second, word, words);
    }
}

TrieTree::~TrieTree()
{
    std::queue<TrieNode*> q;
    q.push(m_root);
    while(!q.empty())
    {
        TrieNode* node = q.front();
        q.pop();
        for (auto& pair : node->m_childMap)
        {
            q.push(pair.second);
        }
        delete node;
    }
}

}