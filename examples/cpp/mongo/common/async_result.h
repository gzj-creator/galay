#ifndef GALAY_MONGO_EXAMPLE_ASYNC_RESULT_H
#define GALAY_MONGO_EXAMPLE_ASYNC_RESULT_H

#include <expected>
#include <string>
#include <utility>

#include <galay/cpp/galay-kernel/core/task.h>

#include <galay/cpp/galay-mongo/base/mongo_error.h>

namespace mongo_example
{

template <typename T>
std::expected<T, galay::mongo::MongoError>
unwrapMongoTaskResult(std::expected<std::expected<T, galay::mongo::MongoError>,
                                    galay::kernel::detail::TaskResultError>&& task_result,
                      galay::mongo::MongoErrorType fallback = galay::mongo::MONGO_ERROR_INTERNAL)
{
    if (!task_result) {
        return std::unexpected(galay::mongo::MongoError(
            fallback,
            std::string(task_result.error().message())));
    }
    return std::move(task_result.value());
}

} // namespace mongo_example

#endif // GALAY_MONGO_EXAMPLE_ASYNC_RESULT_H
