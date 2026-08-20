#include "config.h"

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-postgres/async/client.h>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <utility>

using namespace std::chrono_literals;

namespace
{

using galay::kernel::IOScheduler;
using galay::kernel::Runtime;
using galay::kernel::RuntimeBuilder;
using galay::kernel::Task;
using galay::postgres::AsyncPostgresClient;
using galay::postgres::PostgresConfig;

Task<int> runTransaction(IOScheduler* scheduler, PostgresConfig config)
{
    AsyncPostgresClient<> client(scheduler);
    auto connected = co_await client.connect(std::move(config)).timeout(5s);
    if (!connected || !connected->has_value() || !connected->value() ||
        client.transactionStatus() != 'I') {
        std::cerr << "connect did not finish at ReadyForQuery(I)\n";
        co_return 1;
    }

    auto begun = co_await client.beginTransaction().timeout(5s);
    if (!begun || !begun->has_value() || client.transactionStatus() != 'T') {
        std::cerr << "BEGIN did not finish at ReadyForQuery(T)\n";
        co_return 2;
    }

    auto failed = co_await client.query("SELECT 1 / 0").timeout(5s);
    if (failed || failed.error().sqlState() != "22012" ||
        client.transactionStatus() != 'E') {
        std::cerr << "failed transaction did not finish at ReadyForQuery(E)\n";
        co_return 3;
    }

    auto rolled_back = co_await client.rollback().timeout(5s);
    if (!rolled_back || !rolled_back->has_value() || client.transactionStatus() != 'I') {
        std::cerr << "ROLLBACK did not restore ReadyForQuery(I)\n";
        co_return 4;
    }

    auto second_begin = co_await client.beginTransaction().timeout(5s);
    if (!second_begin || !second_begin->has_value() || client.transactionStatus() != 'T') {
        std::cerr << "second BEGIN did not enter transaction state\n";
        co_return 5;
    }
    auto inside = co_await client.query("SELECT 9").timeout(5s);
    if (!inside || !inside->has_value() || inside->value().rowCount() != 1 ||
        inside->value().row(0).getInt64(0, -1) != 9 || client.transactionStatus() != 'T') {
        std::cerr << "query inside transaction failed\n";
        co_return 6;
    }
    auto committed = co_await client.commit().timeout(5s);
    if (!committed || !committed->has_value() || client.transactionStatus() != 'I') {
        std::cerr << "COMMIT did not restore ReadyForQuery(I)\n";
        co_return 7;
    }

    auto closed = co_await client.close();
    if (!closed) {
        std::cerr << "PostgreSQL close failed\n";
        co_return 8;
    }
    co_return 0;
}

} // namespace

int main()
{
    auto config = galay::postgres::test::integrationConfig();
    if (!config) {
        std::cerr << "t8_transaction skipped: set GALAY_IT_ENABLE=1 and "
                     "GALAY_POSTGRES_TEST_{HOST,PORT,USER,PASSWORD,DATABASE}.\n";
        return 125;
    }

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    auto started = runtime.start();
    if (!started) {
        std::cerr << "runtime start failed: " << started.error().message() << '\n';
        return EXIT_FAILURE;
    }
    IOScheduler* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        runtime.stop();
        std::cerr << "runtime has no IO scheduler\n";
        return EXIT_FAILURE;
    }

    auto result = runtime.blockOnIO(runTransaction(scheduler, std::move(*config)));
    runtime.stop();
    if (!result) {
        std::cerr << "runtime blockOn failed: " << result.error().message() << '\n';
        return EXIT_FAILURE;
    }
    return *result;
}
