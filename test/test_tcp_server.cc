#include "galay/kernel/server/TcpServer.h"
#include "galay/utils/BackTrace.h"
#include "galay/utils/SignalHandler.hpp"
#include "galay/common/Buffer.hpp"

using namespace galay;

std::string getSignalName(int sig) {
    switch(sig) {
        case SIGSEGV: return "SIGSEGV (Segmentation Fault)";
        case SIGABRT: return "SIGABRT (Abort)";
        case SIGFPE:  return "SIGFPE (Floating Point Exception)";
        case SIGILL:  return "SIGILL (Illegal Instruction)";
        case SIGBUS:  return "SIGBUS (Bus Error)";
        default:      return "Unknown Signal";
    }
}

void signalHandler(int sig) {
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
    utils::SignalHandler::setSignalHandler<SIGSEGV>(signalHandler);
    TcpServerBuilder builder;
    builder.addListen({"0.0.0.0", 8070});
    TcpServer server = builder.startCoChecker(true, std::chrono::milliseconds(1000)).build();
    server.run([&server](AsyncTcpSocket socket, size_t id) -> Coroutine<nil> {
        std::cout << "connection established" << std::endl;
        Buffer buffer(1024);
        while(true) {
            auto rwrapper = co_await socket.recv(buffer.data(), buffer.capacity());
            if(!rwrapper.success()) {
                if(rwrapper.getError()->code() == error::ErrorCode::DisConnectError) {
                    // disconnect
                    co_await socket.close();
                    std::cout << "connection close" << std::endl;
                    co_return nil();
                }
                std::cout << "recv error: " << rwrapper.getError()->message() << std::endl;
                co_return nil();
            }
            Bytes bytes = rwrapper.moveValue();
            std::string msg = bytes.toString();
            if (msg.find("quit") != std::string::npos)
            {
                auto success = co_await socket.close();
                if (success.success())
                {
                    std::cout << "close success" << std::endl;
                }
                server.stop();
                co_return nil();
            } 
            std::cout << "receive: " << msg << std::endl;
            auto wwrapper = co_await socket.send(std::move(bytes));
            if (wwrapper.success())
            {
                Bytes remain = wwrapper.moveValue();
                std::cout << remain.toString()  << std::endl;
            } else {
                std::cout << "write error: " << wwrapper.getError()->message() << std::endl;
            }
        }
    });
    server.wait();
    return 0;
}