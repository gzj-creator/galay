#ifndef GALAY_TEST_MONGO_ASYNC_RESULT_HELPER_H
#define GALAY_TEST_MONGO_ASYNC_RESULT_HELPER_H

#include <expected>
#include <string>
#include <utility>

#include <galay-kernel/core/task.h>

#include "galay-mongo/base/mongo_error.h"

namespace mongo_test
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

} // namespace mongo_test

#endif // GALAY_TEST_MONGO_ASYNC_RESULT_HELPER_H
