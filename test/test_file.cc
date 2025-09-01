#include "galay/kernel/async/File.h"
#include "galay/kernel/runtime/Runtime.h"
#include "galay/common/Log.h"
#include <iostream>
#include <filesystem>

using namespace galay;

Runtime runtime;

#ifdef USE_AIO  
Coroutine<nil> test()
{
    File file(runtime);
    OpenFlags flags;
    //读写权限
    flags.create().noBlock().readWrite();
    file.open("./test1.txt", flags, FileModes{});
    //初始化异步IO
    file.aioInit(1024);
    StringMetaData wdata("1234");
    //结果收集容器，保证生命周期在 commit 函数调用完成之后
    std::vector<int> results(1024, 0);
    for(int i = 0; i < 1024; i++) {
        file.preWrite(wdata, results[i], i * wdata.size);
    }
    auto success = co_await file.commit();
    //结果收集容器，保证生命周期在 commit 函数调用完成之后
    std::vector<StringMetaData> rdata(1024);
    for(int i = 0; i < 1024; i++) {
        rdata[i] = mallocString(4);
        file.preRead(rdata[i], i * 4);
    }
    co_await file.commit();
    //验证数据
    std::string verify("1234");
    for(auto& by: rdata) {
        if(std::string(reinterpret_cast<char*>(by.data), by.size) != verify) {
            std::cout << "verify failed" << std::endl;
            break;
        } 
        freeString(by);
    }
    std::cout << "verify success" << std::endl;
    co_await file.close();
    std::cout << "close success" << std::endl;
    std::error_code ec;
    if(!std::filesystem::remove("./test1.txt", ec)) {
        std::cout << "remove failed: " << ec.message() << std::endl;
    } else {
        std::cout << "remove success: " << ec.message() << std::endl;
    }
    co_return nil();
}

Coroutine<nil> test_v()
{
    File file(runtime);
    OpenFlags flags;
    flags.create().noBlock().readWrite();
    file.open("./test2.txt", flags, FileModes{});
    file.aioInit(1024);
    std::vector<StringMetaData> wbytes(1024);
    for(int i = 0; i < 1024; ++i){
        wbytes[i] = StringMetaData("1234");
    }
    IOVecResult result;
    file.preWriteV(wbytes, result, 0);
    co_await file.commit();
    std::vector<StringMetaData> rdata(1024);
    for(int i = 0; i < 1024; ++i) {
        rdata[i] = mallocString(4);
    }
    IOVecResult rresult;
    file.preReadV(rdata, rresult, 0);
    co_await file.commit();
    for(auto& v: rdata){
        if(std::string(reinterpret_cast<char*>(v.data), v.size) != "1234") {
            std::cout << "vector verify failed" << std::endl;
            break;
        }
    }
    std::cout << "vector verify success" << std::endl;
    co_await file.close();
    std::cout << "file close success" << std::endl;
    std::error_code ec;
    if(!std::filesystem::remove("./test2.txt", ec)) {
        std::cout << "remove failed: " << ec.message() << std::endl;
    } else {
        std::cout << "remove success: " << ec.message() << std::endl;
    }
    co_return nil();
}

#else

Coroutine<nil> test()
{ 
    std::cout << "testing" << std::endl;
    File file(runtime);
    OpenFlags flags;
    flags.create().noBlock().readWrite();
    auto ret = file.open("./test2.txt", flags, FileModes());
    if(!ret.success()) {
        std::cout << "open failed: " << ret.getError()->message() << std::endl;
        co_return nil();
    }
    std::cout << "open success" << std::endl;
    std::string verify(10240, 'a');
    Bytes bytes = Bytes::fromString(verify);
    auto wwrapper = co_await file.write(std::move(bytes));
    if(!wwrapper.success()) {
        std::cout << "write error: " << wwrapper.getError()->message() << std::endl;
        co_return nil();
    }
    std::cout << "write success: " << wwrapper.moveValue().size() << std::endl;
    file.seek(0);
    auto rwrapper = co_await file.read(10240);
    if(!rwrapper.success()) {
        std::cout << "read error: " << rwrapper.getError()->message() << std::endl;
        co_return nil();
    }
    auto str = rwrapper.moveValue();
    std::cout << "read success: " << str.size() << std::endl;
    if(str == verify) {
        std::cout << "verify success" << std::endl;
    } else {
        std::cout << "verify failed" << std::endl;
    }
    co_await file.close();
    file.remove();
    co_return nil();
}
#endif



int main() { 
    LogTrace("main");
    galay::details::InternelLogger::getInstance()->setLevel(spdlog::level::trace);
    RuntimeBuilder builder;
    builder.startCoManager(std::chrono::milliseconds(1000));
    runtime = builder.build();
    runtime.start();
    runtime.schedule(test());
#ifdef USE_AIO
    runtime.schedule(test_v());
#endif
    getchar();
    runtime.stop();
    return 0;
}