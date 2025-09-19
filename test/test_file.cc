#include "galay/kernel/async/File.h"
#include "galay/common/Buffer.h"
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
    char* buffer = "1234";
    for(int i = 0; i < 1024; i++) {
        file.preWrite(buffer, 4, i * 4);
    }
    auto wexpects = file.commit();
    while(auto events = co_await file.getEvent(wexpects.value())) {
    }
    file.clearIocbs();
    std::cout << std::endl; 
    char rbuffer[1024 * 4] = {0};
    std::cout << "start read" << std::endl;
    for(int i = 0; i < 1024; i++) {
        file.preRead(rbuffer + i * 4, 4, i * 4);
    }
    auto expects = file.commit();
    
    while(auto rdata = co_await file.getEvent(expects.value())) {
    }
    for(int i = 0; i < 1024; i++) {
        std::string_view str(rbuffer + i * 4, 4);
        if(str != "1234") {
            std::cout << str << " - " << i << " ";
            std::cout << "verify failed" << std::endl;
            co_await file.close();
            co_return nil();
        }

    }
    file.clearIocbs();
    //验证数据
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
    if( auto res = file.aioInit(1024); !res) {
        std::cout << "aio init failed" << std::endl;
        std::cout << res.error().message() << std::endl;
        co_await file.close();
        co_return nil();
    }

    char* buffer = "1234";
    std::vector<iovec> wvecs(1024);
    for(int i = 0; i < 1024; ++i){
        wvecs[i].iov_base = buffer;
        wvecs[i].iov_len = 4;
    }
    std::cout << "start write" << std::endl;
    file.preWriteV(wvecs, 0);
    auto expected = file.commit();
    if(!expected) {
        std::cout << "commit error: " << expected.error().message() << std::endl;
        co_await file.close();
        co_return nil();
    } else {
        std::cout << "commit success: " << expected.value() << std::endl;
    }
    while(auto events = co_await file.getEvent(expected.value())) {
        for(auto& event: events.value()) {
            std::cout << event.res << " - " << event.res2 << " ";
        }
    }
    std::cout << std::endl;
    file.clearIocbs();
    Buffer rbuffer(1024 * 4);
    std::vector<iovec> rdata(1024);
    for(int i = 0; i < 1024; ++i) {
        rdata[i].iov_base = ( rbuffer.data() + i * 4);
        rdata[i].iov_len = 4;
    }
    file.preReadV(rdata, 0);
    auto revents = file.commit();
    while(auto rres = co_await file.getEvent(revents.value())) {
    }
    for(int i = 0; i < 1024; i++) {
        std::string_view str(rbuffer.data() + i * 4, 4);
        if(str != "1234") {
            std::cout << "verify failed" << std::endl;
            co_await file.close();
            co_return nil();
        }

    }
    file.clearIocbs();
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
    if(!ret) {
        std::cout << "open failed: " << ret.error().message() << std::endl;
        co_return nil();
    }
    std::cout << "open success" << std::endl;
    std::string verify(10240, 'a');
    Bytes bytes = Bytes::fromString(verify);
    auto wwrapper = co_await file.write(std::move(bytes));
    if(!wwrapper) {
        std::cout << "write error: " << wwrapper.error().message() << std::endl;
        co_return nil();
    }
    std::cout << "write success: " << wwrapper.value().size() << std::endl;
    file.seek(0);
    auto rwrapper = co_await file.read(10240);
    if(!rwrapper) {
        std::cout << "read error: " << rwrapper.error().message() << std::endl;
        co_return nil();
    }
    auto& str = rwrapper.value();
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
    getchar();
#ifdef USE_AIO
    runtime.schedule(test_v());
#endif
    getchar();
    runtime.stop();
    return 0;
}