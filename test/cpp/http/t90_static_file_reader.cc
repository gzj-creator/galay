/**
 * @file t90_static_file_reader.cc
 * @brief Exercises the unified static-file reader through a live runtime.
 */

#include <galay/cpp/galay-http/server/static_file_reader.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <unistd.h>

namespace
{

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << "\n";
    }
    return condition;
}

} // namespace

int main()
{
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("galay-static-reader-" + std::to_string(static_cast<long long>(::getpid())) + ".txt");
    const std::string content = "0123456789abcdefghijklmnopqrstuvwxyz";
    {
        std::ofstream output(path, std::ios::binary);
        if (!check(output.good(), "failed to create static reader fixture")) {
            return 1;
        }
        output << content;
    }

    galay::kernel::Runtime runtime = galay::kernel::RuntimeBuilder()
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .build();
    auto started = runtime.start();
    if (!check(started.has_value(), "failed to start runtime")) {
        std::filesystem::remove(path);
        return 1;
    }

    auto metadata_task = runtime.blockOnIO(galay::http::StaticFileReader::inspect(path.string()));
    if (!check(metadata_task.has_value(), "metadata task failed")) {
        runtime.stop();
        std::filesystem::remove(path);
        return 1;
    }
    auto metadata = std::move(metadata_task.value());
    if (!check(metadata.has_value(), "metadata result failed") ||
        !check(metadata->file_size == content.size(), "metadata size mismatch") ||
        !check(metadata->canonical_path == std::filesystem::canonical(path).string(),
               "metadata canonical path mismatch")) {
        runtime.stop();
        std::filesystem::remove(path);
        return 1;
    }

    auto all_task = runtime.blockOnIO(
        galay::http::StaticFileReader::readAll(path.string(), content.size()));
    if (!check(all_task.has_value(), "readAll task failed")) {
        runtime.stop();
        std::filesystem::remove(path);
        return 1;
    }
    auto all = std::move(all_task.value());
    if (!check(all.has_value(), "readAll result failed") ||
        !check(all.value() == content, "readAll content mismatch")) {
        runtime.stop();
        std::filesystem::remove(path);
        return 1;
    }

    auto range_task = runtime.blockOnIO(
        galay::http::StaticFileReader::readAt(path.string(), 7, 11));
    if (!check(range_task.has_value(), "readAt task failed")) {
        runtime.stop();
        std::filesystem::remove(path);
        return 1;
    }
    auto range = std::move(range_task.value());
    if (!check(range.has_value(), "readAt result failed") ||
        !check(range.value() == content.substr(7, 11), "readAt content mismatch")) {
        runtime.stop();
        std::filesystem::remove(path);
        return 1;
    }

    auto invalid_task = runtime.blockOnIO(
        galay::http::StaticFileReader::readAt(
            path.string(), std::numeric_limits<size_t>::max(), 1));
    if (!check(invalid_task.has_value(), "invalid readAt task failed")) {
        runtime.stop();
        std::filesystem::remove(path);
        return 1;
    }
    auto invalid = std::move(invalid_task.value());
    if (!check(!invalid.has_value(), "invalid readAt unexpectedly succeeded") ||
        !check(invalid.error().code == galay::http::StaticFileReadErrorCode::kInvalidRange,
               "invalid readAt returned the wrong error")) {
        runtime.stop();
        std::filesystem::remove(path);
        return 1;
    }

    auto session_task = runtime.blockOnIO(galay::http::StaticFileReader::open(path.string()));
    if (!check(session_task.has_value(), "open session task failed")) {
        runtime.stop();
        std::filesystem::remove(path);
        return 1;
    }
    auto session_result = std::move(session_task.value());
    if (!check(session_result.has_value(), "open session result failed")) {
        runtime.stop();
        std::filesystem::remove(path);
        return 1;
    }
    auto session = std::move(session_result.value());
    auto session_read_task = runtime.blockOnIO(session.readAt(19, 6));
    if (!check(session_read_task.has_value(), "session read task failed")) {
        runtime.stop();
        std::filesystem::remove(path);
        return 1;
    }
    auto session_read = std::move(session_read_task.value());
    if (!check(session_read.has_value(), "session read result failed") ||
        !check(session_read.value() == content.substr(19, 6), "session read content mismatch")) {
        runtime.stop();
        std::filesystem::remove(path);
        return 1;
    }

    auto descriptor_task = runtime.blockOnIO(
        galay::http::StaticFileReader::openForSendfile(path.string()));
    if (!check(descriptor_task.has_value(), "openForSendfile task failed")) {
        runtime.stop();
        std::filesystem::remove(path);
        return 1;
    }
    auto descriptor = std::move(descriptor_task.value());
    if (!check(descriptor.has_value() && descriptor->valid(),
               "openForSendfile result failed")) {
        runtime.stop();
        std::filesystem::remove(path);
        return 1;
    }

    auto missing_task = runtime.blockOnIO(
        galay::http::StaticFileReader::inspect(path.string() + ".missing"));
    if (!check(missing_task.has_value(), "missing metadata task failed") ||
        !check(!missing_task.value().has_value(), "missing metadata unexpectedly succeeded")) {
        runtime.stop();
        std::filesystem::remove(path);
        return 1;
    }

    descriptor->close();
    runtime.stop();
    std::filesystem::remove(path);
    std::cout << "T90-StaticFileReader PASS\n";
    return 0;
}
