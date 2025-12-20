#include "Waker.h"
#include "Coroutine.hpp"
#include <cstddef>
#include <optional>
#include <variant>

namespace galay
{
    Waker::Waker(CoroutineBase::wptr coroutine)
        :m_coroutine(coroutine), m_data(nullptr)
    {
    }

    void Waker::wakeUp(void* data)
    {
        m_data = data;
        if (auto coroutine = m_coroutine.lock()) {
            coroutine->resume();
        }
    }

    WakerWrapper::WakerWrapper()
    {
    }

    unsigned long WakerWrapper::getTypes() 
    {
        return m_types.to_ulong();
    }

    bool WakerWrapper::contains(WakerType type)
    {
        return m_types[type] == 1;
    }

    bool WakerWrapper::empty()
    {
        return m_types.none();
    }

    void WakerWrapper::delType(WakerType type)
    {
        if (type >= WakerType::SIZE) return;

        if (type == WakerType::READ || type == WakerType::WRITE) {
            // 处理读写并存的情况
            if (std::holds_alternative<std::array<Waker, 2>>(m_wakers)) {
                auto& array = std::get<std::array<Waker, 2>>(m_wakers);
                array[type] = Waker{};
            }
        } else {
            // 处理其他互斥的waker
            m_wakers = Waker{};
        }
        m_types.reset(type);
    }

    std::optional<Waker> WakerWrapper::getWaker(WakerType type)
    {
        if(m_types[type] == 0) return std::nullopt;

        std::optional<Waker> result;

        if (type == WakerType::READ || type == WakerType::WRITE) {
            // 处理读写并存的情况
            if (std::holds_alternative<std::array<Waker, 2>>(m_wakers)) {
                auto& array = std::get<std::array<Waker, 2>>(m_wakers);
                result = std::move(array[type]);
            }
        } else {
            // 处理其他互斥的waker
            if (std::holds_alternative<Waker>(m_wakers)) {
                result = std::move(std::get<Waker>(m_wakers));
            }
        }

        m_types.reset(type);
        return result;
    }

    void WakerWrapper::addType(WakerType type, Waker waker)
    {
        if (type >= WakerType::SIZE) return;

        if (type == WakerType::READ || type == WakerType::WRITE) {
            // 处理读写并存的情况
            if (!std::holds_alternative<std::array<Waker, 2>>(m_wakers)) {
                // 如果当前不是array，创建新的array
                std::array<Waker, 2> newArray{};
                newArray[type] = std::move(waker);
                m_wakers = std::move(newArray);
            } else {
                // 如果已经是array，直接设置对应位置
                auto& array = std::get<std::array<Waker, 2>>(m_wakers);
                array[type] = std::move(waker);
            }
        } else {
            // 处理其他互斥的waker
            m_wakers = std::move(waker);
        }

        m_types.set(type);
    }
    
}
