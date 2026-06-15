#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <galay-kernel/core/runtime.h>

#include "common/async_result.h"
#include "common/config.h"
#include "galay-mongo/async/client.h"

using namespace galay::kernel;
using namespace galay::mongo;

namespace
{

int64_t makeUniqueId()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

MongoDocument makeInsertCommand(const std::string& collection,
                                int64_t id,
                                int32_t counter)
{
    MongoDocument doc;
    doc.append("_id", id);
    doc.append("name", "async-command-example");
    doc.append("counter", counter);

    MongoArray documents;
    documents.append(std::move(doc));

    MongoDocument cmd;
    cmd.append("insert", collection);
    cmd.append("documents", std::move(documents));
    cmd.append("ordered", true);
    return cmd;
}

MongoDocument makeFindCommand(const std::string& collection, int64_t id)
{
    MongoDocument filter;
    filter.append("_id", id);

    MongoDocument cmd;
    cmd.append("find", collection);
    cmd.append("filter", std::move(filter));
    cmd.append("limit", int32_t(1));
    return cmd;
}

MongoDocument makeUpdateCommand(const std::string& collection,
                                int64_t id,
                                int32_t counter)
{
    MongoDocument filter;
    filter.append("_id", id);

    MongoDocument set_doc;
    set_doc.append("counter", counter);

    MongoDocument update_doc;
    update_doc.append("$set", std::move(set_doc));

    MongoDocument update_item;
    update_item.append("q", std::move(filter));
    update_item.append("u", std::move(update_doc));
    update_item.append("multi", false);
    update_item.append("upsert", false);

    MongoArray updates;
    updates.append(std::move(update_item));

    MongoDocument cmd;
    cmd.append("update", collection);
    cmd.append("updates", std::move(updates));
    cmd.append("ordered", true);
    return cmd;
}

MongoDocument makeDeleteCommand(const std::string& collection, int64_t id)
{
    MongoDocument filter;
    filter.append("_id", id);

    MongoDocument delete_item;
    delete_item.append("q", std::move(filter));
    delete_item.append("limit", int32_t(1));

    MongoArray deletes;
    deletes.append(std::move(delete_item));

    MongoDocument cmd;
    cmd.append("delete", collection);
    cmd.append("deletes", std::move(deletes));
    cmd.append("ordered", true);
    return cmd;
}

struct RunState
{
    std::atomic<bool> done{false};
    std::atomic<bool> ok{true};
    std::string error;
};

struct AsyncClientConfig
{
    MongoConfig mongo;
    AsyncMongoConfig async;
};

void setFailure(RunState* state, std::string message)
{
    state->ok.store(false, std::memory_order_relaxed);
    state->error = std::move(message);
    state->done.store(true, std::memory_order_release);
}

Task<void> run(IOScheduler* scheduler,
               RunState* state,
               AsyncClientConfig cfg)
{
    auto client = AsyncMongoClientBuilder().scheduler(scheduler).config(cfg.async).build();

    const std::string collection = "galay_mongo_example_async_command_crud";
    const int64_t doc_id = makeUniqueId();

    const std::expected<bool, MongoError> conn_result =
        mongo_example::unwrapMongoTaskResult(co_await client.connect(cfg.mongo),
                                             MONGO_ERROR_CONNECTION);
    if (!conn_result) {
        setFailure(state, "connect failed: " + conn_result.error().message());
        co_return;
    }

    const std::expected<MongoReply, MongoError> inserted =
        mongo_example::unwrapMongoTaskResult(
            co_await client.command(cfg.mongo.database, makeInsertCommand(collection, doc_id, 1)),
            MONGO_ERROR_COMMAND);
    if (!inserted) {
        setFailure(state, "insert failed: " + inserted.error().message());
        co_return;
    }

    const std::expected<MongoReply, MongoError> found =
        mongo_example::unwrapMongoTaskResult(
            co_await client.command(cfg.mongo.database, makeFindCommand(collection, doc_id)),
            MONGO_ERROR_COMMAND);
    if (!found) {
        setFailure(state, "find failed: " + found.error().message());
        co_return;
    }

    const std::expected<MongoReply, MongoError> updated =
        mongo_example::unwrapMongoTaskResult(
            co_await client.command(cfg.mongo.database, makeUpdateCommand(collection, doc_id, 2)),
            MONGO_ERROR_COMMAND);
    if (!updated) {
        setFailure(state, "update failed: " + updated.error().message());
        co_return;
    }

    const std::expected<MongoReply, MongoError> deleted =
        mongo_example::unwrapMongoTaskResult(
            co_await client.command(cfg.mongo.database, makeDeleteCommand(collection, doc_id)),
            MONGO_ERROR_COMMAND);
    if (!deleted) {
        setFailure(state, "delete failed: " + deleted.error().message());
        co_return;
    }

    co_await client.close();
    state->done.store(true, std::memory_order_release);
}

} // namespace

int main()
{
    const auto mongo_cfg = mongo_example::loadMongoConfigFromEnv();
    const auto async_cfg = mongo_example::loadAsyncMongoConfigFromEnv();

    Runtime runtime;
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "No scheduler available" << std::endl;
        runtime.stop();
        return 1;
    }

    RunState state;
    if (!scheduleTask(scheduler, run(scheduler, &state, AsyncClientConfig{mongo_cfg, async_cfg}))) {
        std::cerr << "Failed to schedule async command CRUD task" << std::endl;
        runtime.stop();
        return 1;
    }

    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!state.done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }

    runtime.stop();

    if (!state.done.load(std::memory_order_acquire)) {
        std::cerr << "Async command CRUD timeout" << std::endl;
        return 1;
    }

    if (!state.ok.load(std::memory_order_relaxed)) {
        std::cerr << "Async command CRUD failed: " << state.error << std::endl;
        return 1;
    }

    std::cout << "Async command CRUD example OK" << std::endl;
    return 0;
}
