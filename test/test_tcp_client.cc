#include "galay/kernel/client/TcpClient.h"
#include "galay/kernel/runtime/Runtime.h"
#include "galay/common/Buffer.h"
#include <iostream>

using namespace galay;

std::mutex mtx;
std::condition_variable cond;

Coroutine<nil> test(Runtime& runtime)
{
    auto handle = runtime.getCoSchedulerHandle(0).value();
    TcpClient client(handle);
    auto res1 = co_await client.connect({"127.0.0.1", 8070});
    if (!res1) {
        std::cout << "connect error: " << res1.error().message() << std::endl;
        cond.notify_one();
        co_return nil();
    }
    std::cout << "connect success" << std::endl;
    Buffer buffer(1024);
    while (true) { 
        std::string msg;
        std::getline(std::cin, msg);
        auto res2 = co_await client.send(Bytes::fromString(msg));
        if (!res2) {
            std::cout << "send error: " << res2.error().message() << std::endl;
            co_return nil();
        }
        std::cout << "send success" << std::endl;
        if(msg == "quit") {
            co_await client.close();
            cond.notify_one();
            co_return nil();
        }
        auto res3 = co_await client.recv(buffer.data(), buffer.capacity());
        if (!res3) {
            std::cout << "recv error: " << res3.error().message() << std::endl;
            co_await client.close();
            co_return nil();
        }
        std::cout << "recv: " << res3.value().toString() << std::endl;
    }

}

int main()
{
    Runtime runtime = RuntimeBuilder().build();
    runtime.start();
    runtime.schedule(test(runtime));
    std::unique_lock lock(mtx);
    cond.wait(lock);
    runtime.stop();
    return 0;
}