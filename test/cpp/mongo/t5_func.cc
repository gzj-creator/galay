#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <galay/cpp/galay-kernel/core/runtime.h>

#include <galay/cpp/galay-mongo/async/client.h>
#include "async_result_helper.h"
#include "config.h"
#include "reply_helper.h"

using namespace galay::kernel;
using namespace galay::mongo;

namespace
{

int64_t make_unique_id()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

MongoDocument make_insert_command(const std::string& collection,
                                int64_t id,
                                int32_t counter,
                                const std::string& stage)
{
    MongoDocument doc;
    doc.append("_id", id);
    doc.append("name", "async-functional");
    doc.append("counter", counter);
    doc.append("stage", stage);

    MongoArray documents;
    documents.append(std::move(doc));

    MongoDocument cmd;
    cmd.append("insert", collection);
    cmd.append("documents", std::move(documents));
    cmd.append("ordered", true);
    return cmd;
}

MongoDocument make_find_command(const std::string& collection, int64_t id)
{
    MongoDocument filter;
    filter.append("_id", id);

    MongoDocument cmd;
    cmd.append("find", collection);
    cmd.append("filter", std::move(filter));
    cmd.append("limit", int32_t(1));
    return cmd;
}

MongoDocument make_update_command(const std::string& collection,
                                int64_t id,
                                int32_t counter,
                                const std::string& stage)
{
    MongoDocument filter;
    filter.append("_id", id);

    MongoDocument set_doc;
    set_doc.append("counter", counter);
    set_doc.append("stage", stage);

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

MongoDocument make_delete_command(const std::string& collection, int64_t id)
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

struct AsyncFunctionalState
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

void set_failure(AsyncFunctionalState* state, std::string message)
{
    state->ok.store(false, std::memory_order_relaxed);
    state->error = std::move(message);
    state->done.store(true, std::memory_order_release);
}

Task<void> run_async_functional(IOScheduler* scheduler,
                              AsyncFunctionalState* state,
                              AsyncClientConfig cfg)
{
    auto client = AsyncMongoClientBuilder().scheduler(scheduler).config(cfg.async).build();

    const std::string database = cfg.mongo.database;
    const std::string collection = "galay_mongo_async_functional";
    const int64_t doc_id = make_unique_id();

    const std::expected<bool, MongoError> connected =
        mongo_test::unwrap_mongo_task_result(co_await client.connect(cfg.mongo),
                                          MONGO_ERROR_CONNECTION);
    if (!connected) {
        set_failure(state, "connect failed: " + connected.error().message());
        co_return;
    }

    const std::expected<MongoReply, MongoError> ping =
        mongo_test::unwrap_mongo_task_result(co_await client.ping(database),
                                          MONGO_ERROR_COMMAND);
    if (!ping) {
        set_failure(state, "ping failed: " + ping.error().message());
        co_return;
    }

    MongoDocument ping_cmd;
    ping_cmd.append("ping", int32_t(1));
    const std::expected<MongoReply, MongoError> ping_by_command =
        mongo_test::unwrap_mongo_task_result(co_await client.command(database, std::move(ping_cmd)),
                                          MONGO_ERROR_COMMAND);
    if (!ping_by_command) {
        set_failure(state, "command(ping) failed: " + ping_by_command.error().message());
        co_return;
    }

    MongoDocument invalid_cmd;
    invalid_cmd.append("galayUnknownCommand", int32_t(1));
    const std::expected<MongoReply, MongoError> invalid_reply =
        mongo_test::unwrap_mongo_task_result(co_await client.command(database, std::move(invalid_cmd)),
                                          MONGO_ERROR_COMMAND);
    if (invalid_reply) {
        set_failure(state, "invalid command should fail but succeeded");
        co_return;
    }
    if (invalid_reply.error().type() != MONGO_ERROR_SERVER) {
        set_failure(state, "invalid command failed, but error type is not MONGO_ERROR_SERVER");
        co_return;
    }

    const std::expected<std::vector<MongoPipelineResponse>, MongoError> empty_pipeline =
        mongo_test::unwrap_mongo_task_result(co_await client.pipeline(database, {}),
                                          MONGO_ERROR_COMMAND);
    if (empty_pipeline) {
        set_failure(state, "empty pipeline should fail but succeeded");
        co_return;
    }
    if (empty_pipeline.error().type() != MONGO_ERROR_INVALID_PARAM) {
        set_failure(state, "empty pipeline failed, but error type is not MONGO_ERROR_INVALID_PARAM");
        co_return;
    }

    std::vector<MongoDocument> commands;
    commands.reserve(3);

    MongoDocument c1;
    c1.append("ping", int32_t(1));
    commands.push_back(std::move(c1));

    MongoDocument c2;
    c2.append("galayUnknownCommand", int32_t(1));
    commands.push_back(std::move(c2));

    MongoDocument c3;
    c3.append("ping", int32_t(1));
    commands.push_back(std::move(c3));

    const std::expected<std::vector<MongoPipelineResponse>, MongoError> mixed_pipeline =
        mongo_test::unwrap_mongo_task_result(co_await client.pipeline(database, std::move(commands)),
                                          MONGO_ERROR_COMMAND);
    if (!mixed_pipeline) {
        set_failure(state, "mixed pipeline failed: " + mixed_pipeline.error().message());
        co_return;
    }
    if (mixed_pipeline->size() != 3) {
        set_failure(state, "mixed pipeline response size is not 3");
        co_return;
    }

    size_t ok_count = 0;
    size_t err_count = 0;
    for (const auto& item : *mixed_pipeline) {
        if (item.request_id <= 0) {
            set_failure(state, "mixed pipeline has invalid request_id");
            co_return;
        }

        if (item.reply.has_value()) {
            ++ok_count;
        } else if (item.error.has_value()) {
            ++err_count;
        } else {
            set_failure(state, "mixed pipeline item has neither reply nor error");
            co_return;
        }
    }
    if (ok_count != 2 || err_count != 1) {
        set_failure(state, "mixed pipeline success/error distribution mismatch");
        co_return;
    }

    const std::expected<MongoReply, MongoError> inserted =
        mongo_test::unwrap_mongo_task_result(
            co_await client.command(database, make_insert_command(collection, doc_id, 1, "created")),
            MONGO_ERROR_COMMAND);
    if (!inserted) {
        set_failure(state, "insert command failed: " + inserted.error().message());
        co_return;
    }

    const std::expected<MongoReply, MongoError> found1 =
        mongo_test::unwrap_mongo_task_result(
            co_await client.command(database, make_find_command(collection, doc_id)),
            MONGO_ERROR_COMMAND);
    if (!found1) {
        set_failure(state, "find command(after insert) failed: " + found1.error().message());
        co_return;
    }

    const auto first_batch_size_1 = mongo_test::first_batch_size(*found1);
    if (!first_batch_size_1) {
        set_failure(state, "find command(after insert) parse failed: " + first_batch_size_1.error());
        co_return;
    }
    if (*first_batch_size_1 != 1) {
        set_failure(state, "find command(after insert) expected firstBatch size=1");
        co_return;
    }

    const auto first_doc_1 = mongo_test::first_batch_front_document(*found1);
    if (!first_doc_1) {
        set_failure(state, "find command(after insert) first document parse failed: " +
                          first_doc_1.error());
        co_return;
    }
    if (first_doc_1->get_int32("counter", -1) != 1 ||
        first_doc_1->get_string("stage") != "created") {
        set_failure(state, "find command(after insert) content mismatch");
        co_return;
    }

    const std::expected<MongoReply, MongoError> updated =
        mongo_test::unwrap_mongo_task_result(
            co_await client.command(database, make_update_command(collection, doc_id, 2, "updated")),
            MONGO_ERROR_COMMAND);
    if (!updated) {
        set_failure(state, "update command failed: " + updated.error().message());
        co_return;
    }

    const std::expected<MongoReply, MongoError> found2 =
        mongo_test::unwrap_mongo_task_result(
            co_await client.command(database, make_find_command(collection, doc_id)),
            MONGO_ERROR_COMMAND);
    if (!found2) {
        set_failure(state, "find command(after update) failed: " + found2.error().message());
        co_return;
    }

    const auto first_doc_2 = mongo_test::first_batch_front_document(*found2);
    if (!first_doc_2) {
        set_failure(state, "find command(after update) first document parse failed: " +
                          first_doc_2.error());
        co_return;
    }
    if (first_doc_2->get_int32("counter", -1) != 2 ||
        first_doc_2->get_string("stage") != "updated") {
        set_failure(state, "find command(after update) content mismatch");
        co_return;
    }

    const std::expected<MongoReply, MongoError> deleted =
        mongo_test::unwrap_mongo_task_result(
            co_await client.command(database, make_delete_command(collection, doc_id)),
            MONGO_ERROR_COMMAND);
    if (!deleted) {
        set_failure(state, "delete command failed: " + deleted.error().message());
        co_return;
    }

    const std::expected<MongoReply, MongoError> found3 =
        mongo_test::unwrap_mongo_task_result(
            co_await client.command(database, make_find_command(collection, doc_id)),
            MONGO_ERROR_COMMAND);
    if (!found3) {
        set_failure(state, "find command(after delete) failed: " + found3.error().message());
        co_return;
    }

    const auto first_batch_size_3 = mongo_test::first_batch_size(*found3);
    if (!first_batch_size_3) {
        set_failure(state, "find command(after delete) parse failed: " + first_batch_size_3.error());
        co_return;
    }
    if (*first_batch_size_3 != 0) {
        set_failure(state, "find command(after delete) expected firstBatch size=0");
        co_return;
    }

    co_await client.close();
    state->done.store(true, std::memory_order_release);
}

} // namespace

int main()
{
    std::cout << "=== T5: Async Mongo Functional Tests ===" << std::endl;

    const auto test_cfg = mongo_test::load_mongo_test_config();
    mongo_test::print_mongo_test_config(test_cfg);

    Runtime runtime;
    runtime.start();

    auto* scheduler = runtime.get_next_io_scheduler();
    if (scheduler == nullptr) {
        std::cerr << "No scheduler available" << std::endl;
        runtime.stop();
        return 1;
    }

    AsyncFunctionalState state;
    if (!schedule_task(scheduler,
                      run_async_functional(scheduler,
                                         &state,
                                         AsyncClientConfig{
                                             mongo_test::to_mongo_config(test_cfg),
                                             mongo_test::load_async_mongo_test_config()}))) {
        std::cerr << "Failed to schedule async functional task" << std::endl;
        runtime.stop();
        return 1;
    }

    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + 20s;
    while (!state.done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(50ms);
    }

    runtime.stop();

    if (!state.done.load(std::memory_order_acquire)) {
        std::cerr << "Async functional timeout" << std::endl;
        return 1;
    }

    if (!state.ok.load(std::memory_order_relaxed)) {
        std::cerr << "Async functional failed: " << state.error << std::endl;
        return 1;
    }

    std::cout << "T5 async functional test OK" << std::endl;
    return 0;
}
