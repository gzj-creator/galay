#include "Proccess.h"

namespace galay::proccess
{
    int Proccess::wait(pid_t pid, int options)
    {
        int status;
        waitpid(pid, &status, options);
        return status;
    }

    pid_t Proccess::spawn(const std::string &path, const std::vector<std::string> &args)
    {
        pid_t pid = fork();
        if (pid == 0) { // 子进程
            // 准备参数数组
            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(path.c_str()));
            for (const auto& arg : args) {
                argv.push_back(const_cast<char*>(arg.c_str()));
            }
            argv.push_back(nullptr);
            
            // 执行程序
            execvp(path.c_str(), argv.data());
            
            // 如果execvp返回，说明出错了
            exit(EXIT_FAILURE);
        } else if (pid > 0) { // 父进程
            return pid;
        } else { // fork失败
            return -1;
        }
    }
}
