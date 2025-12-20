#ifndef GALAY_WAKER_H
#define GALAY_WAKER_H

#include <array>
#include <bitset>
#include <cstdint>
#include <memory>
#include <optional>
#include <array>
#include <variant>

namespace galay
{
    class CoroutineBase;

    //must running on scheduler
    class Waker
    {
    public:
        Waker() {}
        Waker(std::weak_ptr<CoroutineBase> coroutine);
        void wakeUp(void* data);
    private:
        void* m_data;
        std::weak_ptr<CoroutineBase> m_coroutine;
    };

    enum WakerType: uint8_t {
        READ        = 0,
        WRITE       = 1,
        FILEWATCH   = 2,
        SIZE
    };

    class WakerWrapper
    {
    public:
        WakerWrapper();
        // 返回当前全量事件epoll需要全部重新注册
        unsigned long getTypes();
        bool contains(WakerType type);
        // 判断是否还有注册事件
        bool empty();
        //删除对应事件
        void delType(WakerType type);
        //已经删除或取出的waker
        std::optional<Waker> getWaker(WakerType type);
        //添加对应事件（kqueue只m_want_types设置标志位）
        void addType(WakerType type, Waker waker);
    private:
        std::bitset<WakerType::SIZE> m_types;
        std::variant<std::array<Waker, 2>,Waker> m_wakers; //READ和WRITE可以并存，arrary[0]是READ，arrary[1]是WRITE, 其他的Waker互斥
    };
}



#endif
