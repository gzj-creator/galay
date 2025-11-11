#include "UdpServer.h"
#include "galay/kernel/async/AsyncFactory.h"
#include "galay/kernel/runtime/Runtime.h"

namespace galay
{

    UdpServer::UdpServer(UdpServer&& server)
    {
        m_host = std::move(server.m_host);
    }

    void UdpServer::listenOn(Host host)
    {
        m_host = std::move(host);
    }

    void UdpServer::run(Runtime& runtime, const AsyncUdpFunc& callback)
    {
        size_t co_num = runtime.coSchedulerSize();
        for(size_t i = 0; i < co_num; ++i) {
            auto handle = runtime.getCoSchedulerHandle(i);
            if(handle.has_value()) {
                AsyncUdpSocket socket = handle.value().getAsyncFactory().getUdpSocket();
                if(auto res = socket.socket(); !res) {
                    throw std::runtime_error(res.error().message());
                }
                HandleOption options = socket.options();
                if(auto res = options.handleReuseAddr(); !res) {
                    throw std::runtime_error(res.error().message());
                }
                if(auto res = options.handleReusePort(); !res) {
                    throw std::runtime_error(res.error().message());
                }
                if(auto res = socket.bind(m_host); !res) {
                    throw std::runtime_error(res.error().message());
                }
                handle.value().spawn(callback(std::move(socket), handle.value()));
            } else {
                throw std::runtime_error("[UdpServer::run] [invalid handle index]");
            }
        }
    }

    void UdpServer::stop()
    {
        m_condition.notify_one();
    }

    void UdpServer::wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condition.wait(lock);
    }

    UdpServer &UdpServer::operator=(UdpServer &&server)
    {
        if(this != &server) {
            m_host = std::move(server.m_host);
        }
        return *this;
    }

    UdpServerBuilder &UdpServerBuilder::addListen(const Host &host)
    {
        m_host = host;
        return *this;
    }

    UdpServer UdpServerBuilder::build()
    {
        UdpServer server;
        server.listenOn(m_host);
        return server;
    }

}