#include "galay/kernel/client/TcpClient.h"

using namespace galay;

std::mutex mtx;
std::condition_variable cond;

Coroutine<nil> test(Runtime& runtime)
{
    TcpClient client(runtime);
    auto res1 = co_await client.connect({"127.0.0.1", 8070});
    if (!res1.success()) {
        std::cout << "connect error: " << res1.getError()->message() << std::endl;
        co_return nil();
    }
    std::cout << "connect success" << std::endl;
    while (true) { 
        std::string msg;
        std::getline(std::cin, msg);
        auto res2 = co_await client.send(Bytes::fromString(msg));
        if (!res2.success()) {
            std::cout << "send error: " << res2.getError()->message() << std::endl;
            co_return nil();
        }
        std::cout << "send success" << std::endl;
        if(msg == "quit") {
            co_await client.close();
            cond.notify_one();
            co_return nil();
        }
        auto res3 = co_await client.recv(1024);
        if (!res3.success()) {
            std::cout << "recv error: " << res3.getError()->message() << std::endl;
            co_return nil();
        }
        std::cout << "recv: " << res3.moveValue().toString() << std::endl;
    }

}

int main()
{
    Runtime runtime;
    runtime.start();
    runtime.schedule(test(runtime));
    std::unique_lock lock(mtx);
    cond.wait(lock);
    runtime.stop();
    return 0;
}