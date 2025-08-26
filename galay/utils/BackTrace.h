#ifndef GALAY_KERNEL_BACK_TRACE_H
#define GALAY_KERNEL_BACK_TRACE_H 

#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <execinfo.h>  // for backtrace
#include <cxxabi.h>    // for demangling
#include <signal.h>    // for signal handling
#include <stdlib.h>
#include <unistd.h>

namespace galay::utils 
{ 

    class BackTrace {
    public:
        static std::vector<std::string> getStackTrace(int max_frames = 64);
        
        // 打印调用栈到标准错误
        static void printStackTrace();

    private:
         // 符号反修饰（demangle）
        static std::string demangleSymbol(const char* symbol);
    };


}


#endif