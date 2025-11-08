#include "galay/kernel/async/TimerGenerator.h"
#include "galay/kernel/runtime/Runtime.h"
#include "galay/common/Log.h"
#include <iostream>

using namespace galay;

Runtime runtime;
CoSchedulerHandle main_holder;


Coroutine<nil> test()
{   
     auto handle = runtime.getCoSchedulerHandle(0).value();
    TimerGenerator generator(handle);
    TimerGenerator generator1(handle);
    //TimerGenerator::ptr generator2 = TimerGenerator::createPtr(handle);
    /*

        auto res = co_await generator.timeout<nil>(std::chrono::milliseconds(5000), [](){
            //错误 generator会析构
            TimerGenerator generator(runtime);
            generator.init();
            return generator.sleep(std::chrono::milliseconds(10000));
        });
        if(res) {
            std::cout << "exec success" << std::endl;
        } else {
            std::cout << res.error().message() << std::endl;
        }
    */
    auto func = [generator1]() mutable {
        std::cout << "ready sleep" << std::endl;
        return generator1.sleep(std::chrono::milliseconds(5001));
    };
    auto res = co_await generator.timeout<nil>(func, std::chrono::milliseconds(5000));
    if(res) {
        std::cout << "exec success" << std::endl;
    } else {
        std::cout << res.error().message() << std::endl;
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
    main_holder = runtime.schedule(test());
    getchar();
    runtime.stop();
    return 0;
}