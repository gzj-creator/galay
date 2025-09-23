#include "galay/kernel/server/TcpServer.h"
#include "galay/utils/BackTrace.h"
#include "galay/utils/SignalHandler.hpp"
#include "galay/common/Buffer.h"
#include "galay/kernel/async/TimerGenerator.h"

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
    TcpServer server = builder.startCoChecker(std::chrono::milliseconds(1000)).backlog(1024).threads(1).build();
    server.run([&server](AsyncTcpSocket socket, AsyncFactory factory) -> Coroutine<nil> {
        std::cout << "connection established" << std::endl;
        using namespace error;
        Buffer buffer(1024);
        TimerGenerator generator = factory.createTimerGenerator();
        while(true) {
            Holder holder;
            auto twrapper = co_await generator.timeout<std::expected<Bytes, CommonError>>(holder, [&](){
                return socket.recv(buffer.data(), buffer.capacity());
            }, std::chrono::milliseconds(5000)) ;
            if(!twrapper) {
                if(CommonError::contains(twrapper.error().code(), ErrorCode::AsyncTimeoutError)) {
                    std::cout << "timeout" << std::endl;
                    // disconnect
                    holder.destory();
                    continue;
                }
                std::cout << "twrapper error: " << twrapper.error().message() << std::endl;
                co_return nil();
            }
            auto& rwrapper = twrapper.value();
            if (!rwrapper)
            {
                if(CommonError::contains(rwrapper.error().code(), ErrorCode::DisConnectError)) {
                    co_await socket.close();
                    std::cout << "disconnect" << std::endl;
                    co_return nil();
                }
                std::cout << "recv error: " << rwrapper.error().message() << std::endl;
                co_return nil();
            }
            Bytes& bytes = rwrapper.value();
            std::string msg = bytes.toString();
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
            std::cout << "receive: " << msg << std::endl;
            auto wwrapper = co_await socket.send(std::move(bytes));
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