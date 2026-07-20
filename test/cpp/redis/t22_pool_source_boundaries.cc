#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace
{

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input) {
        std::cerr << "failed to open " << path << "\n";
        std::exit(1);
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

std::string bodyAfter(const std::string& text, const std::string& signature)
{
    const auto signature_pos = text.find(signature);
    if (signature_pos == std::string::npos) {
        std::cerr << "missing source boundary signature: " << signature << "\n";
        std::exit(1);
    }

    const auto open_pos = text.find('{', signature_pos);
    if (open_pos == std::string::npos) {
        std::cerr << "missing source boundary body: " << signature << "\n";
        std::exit(1);
    }

    size_t depth = 0;
    for (size_t i = open_pos; i < text.size(); ++i) {
        if (text[i] == '{') {
            ++depth;
        } else if (text[i] == '}') {
            --depth;
            if (depth == 0) {
                return text.substr(open_pos, i - open_pos + 1);
            }
        }
    }

    std::cerr << "unterminated source boundary body: " << signature << "\n";
    std::exit(1);
}

std::filesystem::path repoRoot()
{
    std::filesystem::path file = __FILE__;
    return file.parent_path().parent_path().parent_path().parent_path();
}

bool hasBlockingLockToken(const std::string& text)
{
    return contains(text, "std::mutex") ||
           contains(text, "std::lock_guard") ||
           contains(text, "std::unique_lock");
}

} // namespace

int main()
{
    const auto root = repoRoot();
    const auto client_header = readFile(root / "src/cpp/galay-redis/async/redis_client.h");
    const auto client_source = readFile(root / "src/cpp/galay-redis/async/redis_client.cc");
    const auto client_awaitable_header =
        readFile(root / "src/cpp/galay-redis/details/awaitable.h");
    const auto client_awaitable_inline =
        readFile(root / "src/cpp/galay-redis/details/awaitable.inl");
    const auto pool_header = readFile(root / "src/cpp/galay-redis/async/conn_pool.h");
    const auto pool_awaitable_header =
        readFile(root / "src/cpp/galay-redis/details/pool_awaitable.h");
    const auto pool_awaitable_inline =
        readFile(root / "src/cpp/galay-redis/details/pool_awaitable.inl");
    const auto waiter_state_header = readFile(root / "src/cpp/galay-redis/async/conn_pool_waiter_state.h");
    const auto pool_source = readFile(root / "src/cpp/galay-redis/async/conn_pool.cc");
    const auto session_source = readFile(root / "src/cpp/galay-redis/sync/redis_session.cc");

    for (const auto* forbidden : {
             "m_pool->initializeSync()",
             "m_pool->acquireSync(",
             "std::condition_variable",
             "暂时创建未连接的客户端",
             "No available connections",
         }) {
        if (contains(pool_header, forbidden) || contains(pool_source, forbidden) ||
            contains(pool_awaitable_header, forbidden) ||
            contains(pool_awaitable_inline, forbidden)) {
            std::cerr << "redis pool must not use old blocking/unconnected placeholder path: "
                      << forbidden << "\n";
            return 1;
        }
    }

    if (contains(client_header, "struct RedisExchangeMachine") ||
        contains(client_header, "struct RedisConnectMachine") ||
        contains(client_source, "RedisExchangeMachine<Strategy>::advance") ||
        contains(client_source, "RedisConnectMachine<Strategy>::advance") ||
        !contains(client_awaitable_header, "struct RedisExchangeMachine") ||
        !contains(client_awaitable_header, "struct RedisConnectMachine") ||
        !contains(client_awaitable_inline, "RedisExchangeMachine<Strategy>::advance") ||
        !contains(client_awaitable_inline, "RedisConnectMachine<Strategy>::advance")) {
        std::cerr << "redis client awaitable declarations/definitions must live in details/awaitable files\n";
        return 1;
    }

    for (const auto* awaitable : {
             "class PoolInitializeAwaitable :",
             "class PoolAcquireAwaitable :",
             "class RedissPoolInitializeAwaitable :",
             "class RedissPoolAcquireAwaitable :",
         }) {
        if (contains(pool_header, awaitable) || !contains(pool_awaitable_header, awaitable)) {
            std::cerr << "redis pool awaitable declarations must live in details/pool_awaitable.h: "
                      << awaitable << "\n";
            return 1;
        }
    }
    if (contains(pool_source, "PoolAcquireAwaitable::prepareSuspend") ||
        !contains(pool_awaitable_inline, "PoolAcquireAwaitable::prepareSuspend")) {
        std::cerr << "redis pool awaitable definitions must live in details/pool_awaitable.inl\n";
        return 1;
    }

    const auto redis_prepare_suspend =
        bodyAfter(pool_awaitable_inline, "PoolAcquireAwaitable::prepareSuspend");
    const auto redis_await_resume =
        bodyAfter(pool_awaitable_inline, "PoolAcquireAwaitable::await_resume");
    const auto redis_mark_timeout =
        bodyAfter(pool_awaitable_inline, "PoolAcquireAwaitable::markTimeout");
    const auto redis_await_suspend =
        bodyAfter(pool_awaitable_header, "bool await_suspend(std::coroutine_handle<Promise> handle)");
    const auto rediss_prepare_suspend =
        bodyAfter(pool_awaitable_inline, "RedissPoolAcquireAwaitable::prepareSuspend");
    const auto rediss_await_resume =
        bodyAfter(pool_awaitable_inline, "RedissPoolAcquireAwaitable::await_resume");
    const auto rediss_mark_timeout =
        bodyAfter(pool_awaitable_inline, "RedissPoolAcquireAwaitable::markTimeout");

    for (const auto* forbidden : {"std::mutex", "std::lock_guard", "std::unique_lock"}) {
        if (contains(redis_prepare_suspend, forbidden) ||
            contains(redis_await_resume, forbidden) ||
            contains(redis_mark_timeout, forbidden) ||
            contains(redis_await_suspend, forbidden) ||
            contains(rediss_prepare_suspend, forbidden) ||
            contains(rediss_await_resume, forbidden) ||
            contains(rediss_mark_timeout, forbidden)) {
            std::cerr << "redis pool acquire coroutine path must not use blocking lock token: "
                      << forbidden << "\n";
            return 1;
        }
    }

    if (contains(rediss_prepare_suspend, "acquireSync") ||
        contains(rediss_await_resume, "acquireSync") ||
        contains(rediss_mark_timeout, "acquireSync")) {
        std::cerr << "RedissPoolAcquireAwaitable acquire path must not call acquireSync\n";
        return 1;
    }

    if (!contains(waiter_state_header, "try_complete_waiter") ||
        !contains(pool_source, "try_complete_waiter")) {
        std::cerr << "redis pool waiter timeout/release must use try_complete_waiter ownership transfer\n";
        return 1;
    }

    if (!contains(pool_header, "moodycamel::ConcurrentQueue")) {
        std::cerr << "redis async pool shared state must use non-blocking concurrent queues\n";
        return 1;
    }

    for (const auto* forbidden : {
             "std::queue<std::shared_ptr<PooledConnection>> m_available_connections",
             "std::queue<std::shared_ptr<detail::RedisPoolWaiter>> m_waiters",
             "std::vector<std::shared_ptr<PooledConnection>> m_all_connections",
             "std::queue<std::shared_ptr<PooledRedissConnection>> m_available_connections",
             "std::queue<std::shared_ptr<detail::RedissPoolWaiter>> m_waiters",
             "std::vector<std::shared_ptr<PooledRedissConnection>> m_all_connections",
         }) {
        if (contains(pool_header, forbidden)) {
            std::cerr << "redis async pool must not keep cross-thread shared state in blocking STL containers: "
                      << forbidden << "\n";
            return 1;
        }
    }

    for (const auto* forbidden : {
             "(void)m_pool->wakeOneWaiterFromAvailable()",
             "(void)returnToAvailable",
             "(void)completeOneWaiter",
             "(void)drainAvailableConnections",
             "(void)wakeOneWaiterFromAvailable",
         }) {
        if (contains(pool_source, forbidden)) {
            std::cerr << "redis pool must handle non-void state helper result explicitly: "
                      << forbidden << "\n";
            return 1;
        }
    }

    for (const auto* forbidden : {
             "m_is_initialized = true",
             "m_is_initialized = false",
             "m_is_shutting_down = true",
             "m_is_shutting_down = false",
         }) {
        if (contains(pool_source, forbidden)) {
            std::cerr << "redis pool must update atomic lifecycle flags with store(), not ignored operator=: "
                      << forbidden << "\n";
            return 1;
        }
    }

    for (const auto* forbidden : {
             "m_error.emplace(",
             "m_connect_awaitable.emplace(",
         }) {
        if (contains(pool_source, forbidden) || contains(pool_awaitable_inline, forbidden)) {
            std::cerr << "redis pool must bind and use optional emplace() return values explicitly: "
                      << forbidden << "\n";
            return 1;
        }
    }

    if (!contains(rediss_prepare_suspend, "connect(") &&
        !contains(pool_header, "RedissConnectOperation")) {
        std::cerr << "Rediss pool acquire path must run async connect before returning a new lease\n";
        return 1;
    }

    if (contains(session_source, "return std::unexpected(select_reply.error())")) {
        std::cerr << "selectDB unexpected reply shape must not read expected::error() on value path\n";
        return 1;
    }

    std::cout << "T22-RedisPoolSourceBoundaries PASS\n";
    return 0;
}
