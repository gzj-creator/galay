#include "BackTrace.h"

namespace galay::utils
{
    std::vector<std::string> BackTrace::getStackTrace(int max_frames) {
        std::vector<std::string> stack;
        void* addrlist[max_frames + 1];
        
        // 获取调用栈地址
        int addrlen = backtrace(addrlist, sizeof(addrlist) / sizeof(void*));
        
        if (addrlen == 0) {
            stack.push_back("No stack trace available");
            return stack;
        }
        
        // 将地址转换为可读的字符串
        char** symbollist = backtrace_symbols(addrlist, addrlen);
        
        // 处理每个栈帧
        for (int i = 0; i < addrlen; i++) {
            std::string frame = demangleSymbol(symbollist[i]);
            stack.push_back(frame);
        }
        
        free(symbollist);
        return stack;
    }

    void BackTrace::printStackTrace() {
        std::vector<std::string> stack = getStackTrace();
        std::cerr << "=== Stack Trace ===" << std::endl;
        for (size_t i = 0; i < stack.size(); i++) {
            std::cerr << "[" << i << "] " << stack[i] << std::endl;
        }
        std::cerr << "==================" << std::endl;
    }


    std::string BackTrace::demangleSymbol(const char* symbol) {
        std::string result;
        char* demangled = nullptr;
        int status = -1;
        
        // 尝试提取函数名（如果有的话）
        if (symbol != nullptr) {
            // 格式通常是: 路径(函数名+偏移) [地址]
            std::string symstr(symbol);
            size_t start = symstr.find('(');
            size_t end = symstr.find('+', start);
            
            if (start != std::string::npos && end != std::string::npos) {
                std::string funcname = symstr.substr(start + 1, end - start - 1);
                
                // 尝试反修饰C++函数名
                demangled = abi::__cxa_demangle(funcname.c_str(), nullptr, nullptr, &status);
                
                if (status == 0 && demangled != nullptr) {
                    // 替换原始字符串中的函数名
                    result = symstr.substr(0, start + 1) + demangled + symstr.substr(end);
                    free(demangled);
                    return result;
                }
            }
        }
        
        // 如果无法反修饰，返回原始字符串
        if (demangled != nullptr) {
            free(demangled);
        }
        return symbol != nullptr ? symbol : "null";
    }

}