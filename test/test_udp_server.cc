#include "galay/kernel/server/UdpServer.h"
#include "galay/utils/BackTrace.h"
#include "galay/common/Buffer.h"
#include <signal.h>

using namespace galay;

static std::string getSignalName(int sig) {
    switch(sig) {
        case SIGSEGV: return "SIGSEGV (Segmentation Fault)";
        case SIGABRT: return "SIGABRT (Abort)";
        case SIGFPE:  return "SIGFPE (Floating Point Exception)";
        case SIGILL:  return "SIGILL (Illegal Instruction)";
        case SIGBUS:  return "SIGBUS (Bus Error)";
        default:      return "Unknown Signal";
    }
}

static void signalHandler(int sig) {
    // 打印错误信息
    std::cerr << std::endl << "Received signal " << sig << " (" << getSignalName(sig) << ")" << std::endl;
    
    // 打印调用栈
    utils::BackTrace::printStackTrace();
    
    // 恢复默认处理并重新引发信号（生成core文件）
    signal(SIGSEGV, SIG_DFL);
    raise(sig);
}

int main() 
{
    galay::details::InternelLogger::getInstance()->setLevel(spdlog::level::trace);
    signal(SIGSEGV, signalHandler);
    UdpServerBuilder builder;
    builder.addListen({"0.0.0.0", 8070});
    UdpServer server = builder.startCoChecker(true, std::chrono::milliseconds(1000)).build();
    server.run([&server](AsyncUdpSocket socket, AsyncFactory factory) -> Coroutine<nil> {
        Buffer buffer(1024);
        using namespace error;
        while(true) {
            Host remote;
            auto rwrapper = co_await socket.recvfrom(remote, buffer.data(), buffer.capacity());
            if(!rwrapper) {
                if(CommonError::contains(rwrapper.error().code(), ErrorCode::DisConnectError)) {
                    // disconnect
                    co_await socket.close();
                    std::cout << "connection close" << std::endl;
                    co_return nil();
                }
                std::cout << "recv error: " << rwrapper.error().message() << std::endl;
                co_return nil();
            }
            std::cout << "recv " << remote.ip << ":" << remote.port  << std::endl;
            Bytes& bytes = rwrapper.value();
            std::string msg = bytes.toString();
            std::cout << msg.length() << "   " <<  msg << std::endl;
            if (msg.find("quit") != std::string::npos)
            {
                auto success = co_await socket.close();
                if (success)
                {
                    std::cout << "close success" << std::endl;
                }
                server.stop();
                co_return nil();
            }
            auto wwrapper = co_await socket.sendto(remote, std::move(bytes));
            if (wwrapper)
            {
                Bytes& remain = wwrapper.value();
                std::cout << remain.toString()  << std::endl;
            } else {
                std::cout << "write error: " << wwrapper.error().message() << std::endl;
            }
        }
    });
    server.wait();
    return 0;
}