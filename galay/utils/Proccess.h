#ifndef GALAY_PROCCESS_H
#define GALAY_PROCCESS_H 

#ifdef __linux__
// Linux 特定头文件或代码
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#elif defined(__APPLE__) || defined(__MACH__)
// macOS 特定头文件或代码
#include <sys/sysctl.h>
#include <unistd.h>
#include <sys/wait.h>
#endif
#include <vector>



namespace galay::proccess 
{ 
    class Proccess {
    public:
        // 获取当前进程PID
        static pid_t getProcessId() { return ::getpid(); }
        static int wait(pid_t pid, int options = 0);
        static pid_t spawn(const std::string& path, const std::vector<std::string>& args);
    };
    

}

#endif