#include "galay/kernel/coroutine/AsyncWaiter.hpp"
#include "galay/kernel/runtime/Runtime.h"

using namespace galay;


Coroutine<nil> async_notify_test(AsyncWaiter<void, CommonError>& waiter) {
    std::cout << "async_notify_test" << std::endl;
    waiter.notify({});
    std::cout << "async_notify_test end" << std::endl;
    co_return nil();
}

Coroutine<nil> async_waiter_test() {
    AsyncWaiter<void, CommonError> waiter;
    std::cout << "async_waiter_test" << std::endl;
    waiter.appendTask(async_notify_test(waiter));
    co_await waiter.wait();
    std::cout << "async_waiter_test end" << std::endl;
    co_return nil();
}

Coroutine<nil> async_result_notify_test_1(AsyncWaiter<bool, CommonError>& waiter) { 
    std::cout << "async_result_notify_test_1" << std::endl;
    waiter.notify(true);
    std::cout << "async_result_notify_test end" << std::endl;
    co_return nil();
}

Coroutine<nil> async_result_notify_test_2(AsyncWaiter<bool, CommonError>& waiter) { 
    std::cout << "async_result_notify_test_2" << std::endl;
    waiter.notify(false);
    std::cout << "async_result_notify_test end" << std::endl;
    co_return nil();
}

Coroutine<nil> async_result_waiter_test() {
    AsyncWaiter<bool, CommonError> waiter;
    std::cout << "async_result_waiter_test" << std::endl;
    waiter.appendTask(async_result_notify_test_1(waiter));
    waiter.appendTask(async_result_notify_test_2(waiter));
    auto res = co_await waiter.wait();
    if(!res) {
        std::cout << "error: " << res.error().message() << std::endl;
        co_return nil();
    }
    std::cout << "result: " << std::boolalpha << res.value() << std::endl;
    std::cout << "async_result_waiter_test end" << std::endl;
    co_return nil();
}

int main() { 
    RuntimeBuilder builder;
    Runtime runtime = builder.startCoManager(std::chrono::milliseconds(1000)).build();
    runtime.start();
    runtime.schedule(async_waiter_test());
    getchar();
    runtime.schedule(async_result_waiter_test());
    runtime.stop();
    return 0;
}