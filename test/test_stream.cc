#include "galay/kernel/async/AsyncFactory.h"
#include "galay/kernel/runtime/Runtime.h"

using namespace galay;

Runtime runtime_1;
Runtime runtime_2;

std::condition_variable cond;

std::atomic_bool start = false;

Coroutine<nil> testRead(AsyncTcpSocket socket)
{
    std::cout << "testRead" << std::endl;
    Buffer buffer(1024);
    while (true)
    {
        auto result = co_await socket.recv(buffer.data(), buffer.capacity());
        if(!result) {
            co_return nil();
        }
        std::string msg = result.value().toString();
        std::cout << msg << std::endl;
        if(msg == "start") {
            start = true;
        } else if(msg == "quit") {
            start = false;
            co_await socket.close();
            std::cout << "testRead close" << std::endl;
            co_return nil();
        }
    }
    co_return nil();
}

Coroutine<nil> testWrite(AsyncTcpSocket socket)
{
    std::cout << "testWrite" << std::endl;
    while(!start) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    while(start) {
        co_await socket.send(Bytes::fromString("----------------hello world-----------------"));
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
    }
    co_return nil();
}

Coroutine<nil> test()
{
    AsyncFactory factory1 = runtime_1.getCoSchedulerHandle().getAsyncFactory();
    auto socket = factory1.getTcpSocket();
    socket.socket();
    auto options = socket.options();
    options.handleReuseAddr();
    socket.bind({"127.0.0.1", 8070});
    socket.listen(1024);
    while (true)
    {
        AsyncTcpSocketBuilder builder;
        auto result = socket.accept(builder);
        auto new_socket = builder.build();
        auto handle2 = runtime_2.getCoSchedulerHandle(0).value();
        runtime_2.schedule(testWrite(new_socket.cloneForDifferentRole(handle2)));
        runtime_1.schedule(testRead(std::move(new_socket)));
    }
    
}

int main() { 
    RuntimeBuilder builder;
    builder.startCoManager(std::chrono::milliseconds(1000));
    runtime_1 = builder.build();
    runtime_2 = builder.build();
    runtime_1.start();
    runtime_2.start();
    runtime_1.schedule(test());
    getchar();
    runtime_1.stop();
    runtime_2.stop();
    return 0;
}