#include "TcpServer.h"
#include "galay/kernel/async/AsyncFactory.h"
#include "galay/kernel/runtime/Runtime.h"

namespace galay
{
    TcpServer::TcpServer(TcpServer &&server)
        : m_sockets(std::move(server.m_sockets))
    {
        m_host = std::move(server.m_host);
        m_backlog = server.m_backlog;
    }

    void TcpServer::listenOn(Host host, int backlog)
    {
        m_host = std::move(host);
        m_backlog = backlog;
    }

    bool TcpServer::run(Runtime& runtime, const AsyncTcpFunc &callback)
    {
        if(m_running.load()) {
            LogError("[TcpServer::run] [runtime is running]");
            return false;
        }
        m_running.store(true);
        size_t co_num = runtime.coSchedulerSize();
        for(size_t i = 0; i < co_num; ++i) {
            auto handle = runtime.getCoSchedulerHandle(i);
            if(handle.has_value()) {
                m_sockets.emplace_back(handle.value().getAsyncFactory().getTcpSocket());
                if(auto res = m_sockets[i].socket(); !res) {
                    throw std::runtime_error(res.error().message());
                }
                HandleOption options = m_sockets[i].options();
                if(auto res = options.handleReuseAddr(); !res) {
                    throw std::runtime_error(res.error().message());
                }
                if(auto res = options.handleReusePort(); !res) {
                    throw std::runtime_error(res.error().message());
                }
                if(auto res = m_sockets[i].bind(m_host); !res) {
                    throw std::runtime_error(res.error().message());
                }
                if(auto res = m_sockets[i].listen(m_backlog); !res) {
                    throw std::runtime_error(res.error().message());
                }
                handle.value().spawn(acceptConnection(handle.value(), callback, i));
            } else {
                throw std::runtime_error("[TcpServer::run] [invalid handle index]");
            }
        }
        return true;
    }

    void TcpServer::stop()
    {
        m_running.store(false);
        for(auto& sock : m_sockets) {
            int fd = sock.getHandle().fd;
            if(fd >= 0) {
                // 关闭 socket 会导致挂起的 accept 返回错误
                ::shutdown(fd, SHUT_RDWR);
            }
        }
        m_condition.notify_one();
    }

    void TcpServer::wait()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_condition.wait(lock);
    }

    TcpServer &TcpServer::operator=(TcpServer &&server)
    {
        if(this != &server) {
            m_sockets = std::move(server.m_sockets);
            m_host = std::move(server.m_host);
            m_backlog = server.m_backlog;
        }
        return *this;
    }

    Coroutine<nil> TcpServer::acceptConnection(CoSchedulerHandle handle, AsyncTcpFunc callback, size_t i)
    {
        while(true) {
            AsyncTcpSocketBuilder builder;
            auto acceptor = co_await m_sockets[i].accept(builder);
            if(acceptor) {
                LogInfo("[acceptConnection success]");
                auto socket = builder.build();
                handle.spawn(callback(std::move(socket), handle));
            } else {
                LogError("[acceptConnection failed] [error: {}]", acceptor.error().message());
            }
        }
        co_return nil();
    }

    TcpServerBuilder &TcpServerBuilder::addListen(const Host &host)
    {
        m_host = host;
        return *this;
    }

    TcpServerBuilder &TcpServerBuilder::backlog(int backlog)
    {
        m_backlog = backlog;
        return *this;
    }
    
    TcpServer TcpServerBuilder::build()
    {
        TcpServer server;
        server.listenOn(m_host, m_backlog);
        return server;
    }
}