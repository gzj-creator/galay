#include "galay/kernel/client/TcpSslClient.h"
#include "galay/kernel/runtime/Runtime.h"
#include "galay/common/Buffer.h"

using namespace galay;

std::mutex mtx;
std::condition_variable cond;

Coroutine<nil> test(Runtime& runtime)
{
    auto handle = runtime.getCoSchedulerHandle(0).value();
    TcpSslClient client(handle);
    
    auto res = co_await client.connect({"127.0.0.1", 8070});
    if (!res) {
        std::cout << "connect error: " << res.error().message() << std::endl;
        co_return nil();
    }
    std::cout << "connect success" << std::endl;
    client.readyToSslConnect();
    while (true)
    {
        std::cout << "sslConnect" << std::endl;
        auto result = co_await client.sslConnect();
        if(!result) {
            std::cout << "ssl connect error: " << result.error().message() << std::endl;
            co_return nil();
        }
        if(result.value()) {
            std::cout << "ssl connect success" << std::endl;
            break;
        }
    }
    Buffer buffer(1024);
    while (true) { 
        std::string msg;
        std::getline(std::cin, msg);
        auto res1 = co_await client.sslSend(Bytes::fromString(msg));
        if (!res1) {
            std::cout << "send error: " << res1.error().message() << std::endl;
            co_return nil();
        }
        std::cout << "send success" << std::endl;
        if(msg == "quit") {
            co_await client.close();
            cond.notify_one();
            co_return nil();
        }
        auto res2 = co_await client.sslRecv(buffer.data(), buffer.capacity());
        if (!res2) {
            std::cout << "recv error: " << res2.error().message() << std::endl;
            co_return nil();
        }
        std::cout << "recv: " << res2.value().toString() << std::endl;
    }

}

int main()
{
    // SSL context is now managed by TcpSslClient
    Runtime runtime = RuntimeBuilder().build();
    runtime.start();
    runtime.schedule(test(runtime));
    std::unique_lock lock(mtx);
    cond.wait(lock);
    runtime.stop();
    return 0;
}