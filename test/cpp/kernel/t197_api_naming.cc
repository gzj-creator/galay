#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>
#include <galay/cpp/galay-kernel/async/async_tcp.h>
#include <galay/cpp/galay-utils/encoding/base64.hpp>

#include <cassert>
#include <expected>
#include <iostream>
#include <type_traits>

using galay::kernel::Runtime;
using galay::kernel::RuntimeBuilder;
using galay::kernel::IOError;

template <typename T>
concept HasSnakeRuntime = requires(T& runtime) {
    runtime.get_next_io_scheduler();
    runtime.get_next_parallel_scheduler();
    runtime.get_io_scheduler(0);
    runtime.is_running();
};

template <typename T>
concept HasLegacyRuntime = requires(T& runtime) {
    runtime.getNextIOScheduler();
};

template <typename T>
concept HasSnakeBuilder = requires(T& builder) {
    builder.io_scheduler_count(1);
    builder.parallel_scheduler_count(0);
};

template <typename T>
concept HasLegacyBuilder = requires(T& builder) {
    builder.ioSchedulerCount(1);
};

template <typename T>
concept HasLegacySocketOption = requires(T& socket) {
    socket.option().handleNonBlock();
};

template <typename T>
concept HasLegacyBase64 = requires {
    T::Base64Encode("galay");
};

static_assert(HasSnakeRuntime<Runtime>);
static_assert(!HasLegacyRuntime<Runtime>);
static_assert(HasSnakeBuilder<RuntimeBuilder>);
static_assert(!HasLegacyBuilder<RuntimeBuilder>);
static_assert(!HasLegacySocketOption<galay::async::AsyncTcpSocket>);
static_assert(!HasLegacyBase64<galay::utils::Base64Util>);
static_assert(requires(galay::async::AsyncTcpSocket& socket) {
    socket.option().handle_non_block();
});

// Scalar IO results retain trivial destruction after the API migration.
static_assert(std::is_trivially_copyable_v<IOError>);
static_assert(std::is_trivially_destructible_v<std::expected<size_t, IOError>>);
static_assert(std::is_trivially_destructible_v<std::expected<void, IOError>>);

int main()
{
    const std::expected<void, IOError> zero_code_error(std::unexpect, galay::kernel::kDisconnectError, 0);
    assert(!zero_code_error && zero_code_error.error().code() == 0);
    std::cout << "IOError=" << sizeof(IOError)
              << " expected<size_t>=" << sizeof(std::expected<size_t, IOError>)
              << " expected<void>=" << sizeof(std::expected<void, IOError>) << '\n';
    assert(galay::utils::Base64Util::base64_encode("galay") == "Z2FsYXk=");
    assert(galay::utils::Base64Util::base64_decode("Z2FsYXk=") == "galay");
    auto runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    const auto answer = runtime.block_on_io([]() -> galay::kernel::Task<int> {
        co_return 42;
    }());
    assert(answer && *answer == 42);
    assert(runtime.is_running());
    assert(runtime.get_next_io_scheduler() != nullptr);
    runtime.stop();
    assert(!runtime.is_running());
}
