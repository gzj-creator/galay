#include "galay/kernel/async/AsyncFactory.h"
#include "galay/kernel/client/TcpClient.h"
#include "galay/kernel/runtime/Runtime.h"

using namespace galay;

Runtime runtime_1;
Runtime runtime_2;

std::atomic_bool start = false;

std::condition_variable cond;
std::mutex mtx;

Coroutine<nil> testRead(TcpClient client)
{
    std::cout << "testRead" << std::endl;
    Buffer buffer(1024);
    while (true)
    {
        auto result = co_await client.recv(buffer.data(), buffer.capacity());
        if(!result) {
            co_return nil();
        }
        std::string msg = result.value().toString();
        std::cout << msg << std::endl;
        if(msg == "start") {
            start = true;
        }
    }
    
}

Coroutine<nil> testWrite(TcpClient client)
{
    std::cout << "testWrite" << std::endl;
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
            cond.notify_all();
            co_return nil();
        }
    }
}

Coroutine<nil> test()
{
    auto handle1 = runtime_1.getCoSchedulerHandle(0).value();
    auto handle2 = runtime_2.getCoSchedulerHandle(0).value();
    TcpClient client(handle1);
    auto res1 = co_await client.connect({"127.0.0.1", 8070});
    if (!res1) {
        std::cout << "connect error: " << res1.error().message() << std::endl;
        co_return nil();
    }
    std::cout << "connect success" << std::endl;
    runtime_2.schedule(testRead(client.cloneForDifferentRole(handle2)));
    runtime_1.schedule(testWrite(std::move(client)));
    co_return nil();
}

int main() { 
    RuntimeBuilder builder;
    builder.startCoManager(std::chrono::milliseconds(1000));
    runtime_1 = builder.build();
    runtime_2 = builder.build();
    runtime_1.start();
    runtime_2.start();
    runtime_1.schedule(test());
    std::unique_lock lock(mtx);
    cond.wait(lock);
    runtime_1.stop();
    runtime_2.stop();
    return 0;
}