#include "galay/kernel/async/File.h"
#include "galay/kernel/runtime/Runtime.h"
#include "galay/common/Log.h"
#include <iostream>
#include <filesystem>

using namespace galay;

Runtime runtime;

Coroutine<nil> test()
{
    File file(runtime);
    OpenFlags flags;
    //读写权限
    flags.create().noBlock().readWrite();
    file.open("./test1.txt", flags, FileModes{});
    //初始化异步IO
    file.aioInit(1024);
    Bytes bytes("1234");
    //结果收集容器，保证生命周期在 commit 函数调用完成之后
    std::vector<int> results(1024, 0);
    for(int i = 0; i < 1024; i++) {
        file.preWrite(bytes, results[i], i * bytes.size());
    }
    auto success = co_await file.commit();
    //结果收集容器，保证生命周期在 commit 函数调用完成之后
    std::vector<Bytes> rbytes(1024);
    for(int i = 0; i < 1024; i++) {
        rbytes[i] = Bytes(4);
        file.preRead(rbytes[i], i * bytes.size());
    }
    co_await file.commit();
    //验证数据
    std::string verify("1234");
    for(auto& by: rbytes) {
        if(by.toString() != verify) {
            std::cout << "verify failed" << std::endl;
            break;
        } else {
        }
    }
    std::cout << "verify success" << std::endl;
    co_await file.close();
    std::cout << "close success" << std::endl;
    std::error_code ec;
    // if(!std::filesystem::remove("./test1.txt", ec)) {
    //     std::cout << "remove failed: " << ec.message() << std::endl;
    // } else {
    //     std::cout << "remove success: " << ec.message() << std::endl;
    // }
    co_return nil();
}

Coroutine<nil> test_v()
{
    File file(runtime);
    OpenFlags flags;
    flags.create().noBlock().readWrite();
    file.open("./test2.txt", flags, FileModes{});
    file.aioInit(1024);
    Bytes bytes("1234");
    std::vector<Bytes*> wbytes(1024);
    for(int i = 0; i < 1024; ++i){
        wbytes[i] = &bytes;
    }
    IOVecResult result;
    file.preWriteV(wbytes, result, 0);
    co_await file.commit();
    std::vector<Bytes> rbytes(1024);
    for(int i = 0; i < 1024; ++i) {
        rbytes[i] = Bytes(4);
    }
    IOVecResult rresult;
    file.preReadV(rbytes, rresult, 0);
    co_await file.commit();
    for(auto& v: rbytes){
        if(v.toString() != "1234") {
            std::cout << "vector verify failed" << std::endl;
            break;
        }
    }
    std::cout << "vector verify success" << std::endl;
    co_await file.close();
    std::cout << "file close success" << std::endl;
    std::error_code ec;
    // if(!std::filesystem::remove("./test2.txt", ec)) {
    //     std::cout << "remove failed: " << ec.message() << std::endl;
    // } else {
    //     std::cout << "remove success: " << ec.message() << std::endl;
    // }
    co_return nil();
}

int main() { 
    LogTrace("main");
    galay::details::InternelLogger::getInstance()->setLevel(spdlog::level::trace);
    RuntimeBuilder builder;
    builder.startCoManager(std::chrono::milliseconds(1000));
    runtime = builder.build();
    runtime.start();
    runtime.schedule(test());
    runtime.schedule(test_v());
    getchar();
    runtime.stop();
    return 0;
}