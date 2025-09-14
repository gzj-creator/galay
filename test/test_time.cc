#include "galay/kernel/async/TimerGenerator.h"
#include "galay/common/Log.h"
#include <iostream>

using namespace galay;

Runtime runtime;
int main_co_id = -1;


Coroutine<nil> test()
{   
    while(main_co_id == -1) {}

    TimerGenerator generator(runtime, main_co_id);
    TimerGenerator generator1(runtime, main_co_id);
    //TimerGenerator::ptr generator2 = TimerGenerator::createPtr(runtime);
    /*

        auto res = co_await generator.timeout<nil>(std::chrono::milliseconds(5000), [](){
            //错误 generator会析构
            TimerGenerator generator(*runtime);
            generator.init();
            return generator.sleep(std::chrono::milliseconds(10000));
        });
        if(res.success()) {
            std::cout << "exec success" << std::endl;
        } else {
            std::cout << res.getError()->message() << std::endl;
        }
    */
    auto func = [generator1]() mutable {
        std::cout << "ready sleep" << std::endl;
        return generator1.sleep(std::chrono::milliseconds(1000));
    };
    auto res = co_await generator.timeout<nil>(std::chrono::milliseconds(5000), func);
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
    main_co_id = runtime.schedule(test());
    getchar();
    runtime.stop();
    return 0;
}