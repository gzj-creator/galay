#include <galay/cpp/galay-utils/buffer/bytes.hpp>
#include <galay/cpp/galay-utils/encoding/base64.hpp>
#include <galay/cpp/galay-utils/common/pool.hpp>

#include <cassert>
#include <limits>
#include <new>
#include <string>

namespace {

struct Resettable {
    int value = 7;
    void reset() { value = 0; }
};

void test_object_pool_lease_can_outlive_pool()
{
    galay::utils::ObjectPool<Resettable>::Ptr lease;
    {
        galay::utils::ObjectPool<Resettable> pool(0, 1);
        lease = pool.acquire();
        assert(lease != nullptr);
        lease->value = 42;
    }

    lease.reset();
}

void test_base64_short_malformed_reports_decode_error()
{
    assert(!galay::utils::Base64Util::base64_can_decode("A"));
    assert(galay::utils::Base64Util::base64_decode("A").empty());
}

void test_base64_crlf_whitespace_decode()
{
    assert(galay::utils::Base64Util::base64_can_decode("SGVs\r\n bG8=\t", true));
    assert(!galay::utils::Base64Util::base64_can_decode("SGVs\r\n bG8=\t", false));
    const std::string decoded = galay::utils::Base64Util::base64_decode("SGVs\r\n bG8=\t", true);
    assert(decoded == "Hello");
    assert(galay::utils::Base64Util::base64_can_decode("\r\n\t ", true));
    assert(!galay::utils::Base64Util::base64_can_decode("\r\n\t ", false));
    assert(galay::utils::Base64Util::base64_decode("\r\n\t ", true).empty());
}

void test_bytes_owned_c_str_is_nul_terminated()
{
    const char raw[] = {'a', 'b', 'c'};
    galay::utils::Bytes bytes(raw, sizeof(raw));

    const char* text = bytes.c_str();
    assert(text != nullptr);
    assert(text[0] == 'a');
    assert(text[1] == 'b');
    assert(text[2] == 'c');
    assert(text[3] == '\0');
}

void test_bytes_terminator_allocation_overflow()
{
    const char byte = 'x';
    for (const auto length : {std::numeric_limits<std::size_t>::max(),
                             static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())}) {
        bool rejected = false;
        try {
            const galay::utils::Bytes bytes(&byte, length);
            assert(bytes.empty());
        } catch (const std::bad_alloc&) {
            rejected = true;
        }
        assert(rejected);
    }
}

} // namespace

int main()
{
    test_object_pool_lease_can_outlive_pool();
    test_base64_short_malformed_reports_decode_error();
    test_base64_crlf_whitespace_decode();
    test_bytes_owned_c_str_is_nul_terminated();
    test_bytes_terminator_allocation_overflow();
    return 0;
}
