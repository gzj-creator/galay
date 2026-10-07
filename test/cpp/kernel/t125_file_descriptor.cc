/**
 * @file t124_file_descriptor.cc
 * @brief 验证 kernel FileDescriptor 的错误传播与所有权语义。
 */

#include <galay/cpp/galay-kernel/common/file_descriptor.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <unistd.h>

namespace
{

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[t124] " << message << '\n';
    }
    return condition;
}

std::string make_temp_file()
{
    char path[] = "/tmp/galay-kernel-file-descriptor-XXXXXX";
    int fd = ::mkstemp(path);
    if (fd < 0) {
        std::cerr << "[t124] mkstemp failed: " << std::strerror(errno) << '\n';
        return {};
    }
    ::close(fd);
    return path;
}

bool test_open_failure_returns_expected_error()
{
    galay::kernel::FileDescriptor fd;
    auto result = fd.open("/tmp/galay-kernel-file-descriptor-missing", O_RDONLY);
    if (!check(!result, "missing file open should fail")) {
        return false;
    }
    if (!check(!fd.valid(), "descriptor should remain invalid after failed open")) {
        return false;
    }
    if (!check(fd.last_error().has_value(), "lastError should store the open failure")) {
        return false;
    }
    return check(galay::kernel::IOError::contains(result.error().code(), galay::kernel::kOpenFailed),
                 "open failure should be reported as kOpenFailed");
}

bool test_open_and_move_transfer_ownership()
{
    const std::string path = make_temp_file();
    if (path.empty()) {
        return false;
    }

    galay::kernel::FileDescriptor fd;
    auto result = fd.open(path.c_str(), O_RDONLY);
    if (!check(result.has_value(), "existing file should open")) {
        ::unlink(path.c_str());
        return false;
    }
    if (!check(fd.valid(), "descriptor should be valid after successful open")) {
        ::unlink(path.c_str());
        return false;
    }

    const int original_fd = fd.get();
    galay::kernel::FileDescriptor moved(std::move(fd));
    bool ok = check(!fd.valid(), "moved-from descriptor should be invalid");
    ok = check(moved.valid(), "move-constructed descriptor should be valid") && ok;
    ok = check(moved.get() == original_fd, "move construction should preserve fd value") && ok;

    galay::kernel::FileDescriptor assigned;
    assigned = std::move(moved);
    ok = check(!moved.valid(), "move-assigned source should be invalid") && ok;
    ok = check(assigned.valid(), "move-assigned destination should be valid") && ok;
    ok = check(assigned.get() == original_fd, "move assignment should preserve fd value") && ok;

    ::unlink(path.c_str());
    return ok;
}

bool test_release_stops_ownership()
{
    const std::string path = make_temp_file();
    if (path.empty()) {
        return false;
    }

    int raw_fd = -1;
    {
        galay::kernel::FileDescriptor fd;
        auto result = fd.open(path.c_str(), O_RDONLY);
        if (!check(result.has_value(), "existing file should open before release")) {
            ::unlink(path.c_str());
            return false;
        }
        raw_fd = fd.release();
        if (!check(raw_fd >= 0, "release should return a valid fd")) {
            ::unlink(path.c_str());
            return false;
        }
        if (!check(!fd.valid(), "descriptor should be invalid after release")) {
            ::close(raw_fd);
            ::unlink(path.c_str());
            return false;
        }
    }

    const bool raw_fd_still_open = ::fcntl(raw_fd, F_GETFD) >= 0;
    ::close(raw_fd);
    ::unlink(path.c_str());
    return check(raw_fd_still_open, "released fd should remain open after wrapper destruction");
}

bool test_open_rejects_when_already_open()
{
    const std::string path = make_temp_file();
    if (path.empty()) {
        return false;
    }

    galay::kernel::FileDescriptor fd;
    auto result = fd.open(path.c_str(), O_RDONLY);
    if (!check(result.has_value(), "existing file should open")) {
        ::unlink(path.c_str());
        return false;
    }

    const int original_fd = fd.get();
    auto rejected = fd.open(path.c_str(), O_RDONLY);
    bool ok = check(!rejected.has_value(), "open while already open should fail");
    ok = check(galay::kernel::IOError::contains(rejected.error().code(), galay::kernel::kAlreadyOpen),
               "open while already open should report kAlreadyOpen") && ok;
    ok = check(fd.valid() && fd.get() == original_fd,
               "rejected open should keep the existing fd") && ok;

    ::unlink(path.c_str());
    return ok;
}

} // namespace

int main()
{
    bool ok = true;
    ok = test_open_failure_returns_expected_error() && ok;
    ok = test_open_and_move_transfer_ownership() && ok;
    ok = test_release_stops_ownership() && ok;
    ok = test_open_rejects_when_already_open() && ok;
    return ok ? 0 : 1;
}
