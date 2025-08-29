#include "galay/kernel/async/TimerGenerator.h"
#include "galay/common/Log.h"
#include <iostream>

using namespace galay;

Runtime runtime;


Coroutine<nil> test()
{
    TimerGenerator generator(runtime);
    /*

        auto res = co_await generator.timeout<nil>(std::chrono::milliseconds(5000), [](){
            //错误 generator会析构
            TimerGenerator generator(*runtime);
            generator.init();
            return generator.sleepfor(std::chrono::milliseconds(10000));
        });
        if(res.success()) {
            std::cout << "exec success" << std::endl;
        } else {
            std::cout << res.getError()->message() << std::endl;
        }
    */
   auto res = co_await generator.timeout<nil>(std::chrono::milliseconds(5000), [generator]() mutable {
        return generator.sleepfor(std::chrono::milliseconds(1000));
    });
    if(res.success()) {
        std::cout << "exec success" << std::endl;
    } else {
        std::cout << res.getError()->message() << std::endl;
    }
    co_return nil();
}

int main()
{
    galay::details::InternelLogger::getInstance()->setLevel(spdlog::level::trace);
    RuntimeBuilder builder;
    builder.startCoManager(std::chrono::milliseconds(1000));
    runtime = builder.build();
    runtime.start();
    runtime.schedule(test());
    getchar();
    runtime.stop();
    return 0;
}