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
        // 移动后重置状态，因为原对象已经不再运行
        m_running.store(false);
    }

    void TcpServer::listenOn(Host host, int backlog)
    {
        m_host = std::move(host);
        m_backlog = backlog;
    }

    bool TcpServer::run(Runtime& runtime, const AsyncTcpFunc &callback)
    {
        bool expected = false;
        if(!m_running.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            LogError("[TcpServer::run] [runtime is running]");
            return false;
        }
        size_t co_num = runtime.coSchedulerSize();
        
        // 预分配容量，避免多次重新分配
        m_sockets.reserve(co_num);
        
        try {
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
                    LogInfo("[TcpServer::run] [listening socket created] [thread: {}] [host: {}:{}]", 
                            i, m_host.ip, m_host.port);
                } else {
                    throw std::runtime_error("[TcpServer::run] [invalid handle index]");
                }
            }
        } catch (...) {
            // 发生异常时清理资源
            m_running.store(false, std::memory_order_release);
            m_sockets.clear();
            throw;
        }
        return true;
    }

    void TcpServer::stop()
    {
        bool expected = true;
        if(!m_running.compare_exchange_strong(expected, false, std::memory_order_acq_rel)) {
            return;  // 已经停止
        }
        
        // 关闭所有 socket，导致挂起的 accept 返回错误
        for(auto& sock : m_sockets) {
            int fd = sock.getHandle().fd;
            if(fd >= 0) {
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
            // 先停止当前服务器
            if(m_running.load(std::memory_order_acquire)) {
                stop();
            }
            m_sockets = std::move(server.m_sockets);
            m_host = std::move(server.m_host);
            m_backlog = server.m_backlog;
            // 移动后重置状态
            m_running.store(false, std::memory_order_release);
        }
        return *this;
    }

    Coroutine<nil> TcpServer::acceptConnection(CoSchedulerHandle handle, AsyncTcpFunc callback, size_t i)
    {
        // 使用局部变量缓存 m_running 状态，减少原子操作
        while(m_running.load(std::memory_order_acquire)) {
            AsyncTcpSocketBuilder builder;
            auto acceptor = co_await m_sockets[i].accept(builder);
            
            // 检查运行状态（使用 acquire 语义确保看到最新的停止状态）
            if(!m_running.load(std::memory_order_acquire)) {
                LogTrace("[acceptConnection stopped after accept] [thread: {}]", i);
                break;
            }
            
            if(acceptor) {
                // 高频日志使用 Trace 级别，减少性能开销
                // 但在调试时可以临时改为 Info 级别
                LogTrace("[acceptConnection success] [thread: {}]", i);
                auto socket = builder.build();
                handle.spawn(callback(std::move(socket), handle));
            } else {
                // 只有在非停止状态下的错误才记录
                if(m_running.load(std::memory_order_acquire)) {
                    LogError("[acceptConnection failed] [thread: {}] [error: {}]", i, acceptor.error().message());
                }
                // 如果是因为 stop() 导致的错误，退出循环
                if(!m_running.load(std::memory_order_acquire)) {
                    break;
                }
            }
        }
        LogTrace("[acceptConnection loop exit] [thread: {}]", i);
        m_condition.notify_all();
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