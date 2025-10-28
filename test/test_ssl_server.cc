#include "galay/kernel/server/TcpSslServer.h"
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
    // 设置日志级别为 trace 以便查看详细日志
    galay::details::InternelLogger::getInstance()->setLevel(spdlog::level::trace);
    
    RuntimeBuilder runtimeBuilder;
    Runtime runtime = runtimeBuilder.build();
    runtime.start();  // 启动 runtime，这是必须的！
    signal(SIGSEGV, signalHandler);
    TcpSslServerBuilder builder("server.crt", "server.key");
    builder.addListen({"0.0.0.0", 8070});
    TcpSslServer server = builder.build();
    server.run(runtime, [&server](AsyncSslSocket socket) -> Coroutine<nil> {
        std::cout << "connection established" << std::endl;
        using namespace error;
        Buffer buffer(1024);
        while(true) {
            //return view
            auto rwrapper = co_await socket.sslRecv(buffer.data(), buffer.capacity());
            if(!rwrapper) {
                if(CommonError::contains(rwrapper.error().code(), ErrorCode::DisConnectError)) {
                    // disconnect
                    co_await socket.sslClose();
                    std::cout << "connection close" << std::endl;
                    co_return nil();
                }
                std::cout << "recv error: " << rwrapper.error().message() << std::endl;
                co_return nil();
            }
            Bytes& bytes = rwrapper.value();
            std::string msg = bytes.toString();
            std::cout << msg.length() << "   " <<  msg << std::endl;
            if (msg.find("quit") != std::string::npos)
            {
                auto success = co_await socket.sslClose();
                if (success)
                {
                    std::cout << "close success" << std::endl;
                }
                server.stop();
                co_return nil();
            }
            auto wwrapper = co_await socket.sslSend(std::move(bytes));
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
    // 等待一小段时间，让协程有机会检查 m_running 标志并退出
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    runtime.stop();
    return 0;
}