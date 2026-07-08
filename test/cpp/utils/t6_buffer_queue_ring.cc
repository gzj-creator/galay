#include "test_common.hpp"

#include <filesystem>

void test_buffer_headers_moved_to_cache() {
    const auto sourceRoot = std::filesystem::path(GALAY_UTILS_SOURCE_DIR);
    assert(!std::filesystem::exists(sourceRoot / "galay-utils/tool/byte_queue_view.hpp"));
    assert(!std::filesystem::exists(sourceRoot / "galay-utils/tool/ring_buffer.hpp"));
    assert(std::filesystem::exists(sourceRoot / "galay-utils/cache/byte_queue_view.hpp"));
    assert(std::filesystem::exists(sourceRoot / "galay-utils/cache/ring_buffer.hpp"));
}

void test_byte_queue_view() {
    std::cout << "=== Testing ByteQueueView ===" << std::endl;

    ByteQueueView queue;
    assert(queue.empty());
    assert(queue.size() == 0);
    assert(queue.has(0));
    assert(!queue.has(1));
    assert(queue.data() == nullptr);
    queue.append(nullptr, 4);
    assert(queue.empty());
    queue.append(std::span<const std::byte>{});
    assert(queue.empty());

    queue.append("hello", 5);
    assert(!queue.empty());
    assert(queue.size() == 5);
    assert(queue.has(5));
    assert(queue.data() != nullptr);
    assert(queue.view(0, 2) == "he");
    assert(queue.view(4, 2).empty());
    assert(queue.view(0, 0).empty());

    queue.consume(2);
    assert(queue.size() == 3);
    assert(queue.view(0, 3) == "llo");

    queue.append(std::string_view(" world"));
    assert(queue.view(0, queue.size()) == "llo world");

    queue.consume(queue.size());
    assert(queue.empty());
    assert(queue.data() == nullptr);
    queue.append("", 0);
    assert(queue.empty());

    std::array<std::byte, 3> bytes{
        std::byte{'a'},
        std::byte{'b'},
        std::byte{'c'}
    };
    queue.append(std::span<const std::byte>(bytes.data(), bytes.size()));
    assert(queue.view(0, queue.size()) == "abc");

    queue.clear();
    std::string large(5000, 'x');
    queue.append(large);
    queue.consume(4096);
    assert(queue.size() == 904);
    assert(queue.view(0, 3) == "xxx");

    queue.append("tail", 4);
    assert(queue.size() == 908);
    assert(queue.view(904, 4) == "tail");

    std::cout << "ByteQueueView tests passed!" << std::endl;
}

// ==================== RingBuffer Tests ====================

void test_ring_buffer() {
    std::cout << "=== Testing RingBuffer ===" << std::endl;

    using DefaultRingBuffer = RingBuffer<>;
    using VectorRingBuffer = RingBuffer<RingBufferBackendStrategy::Vector>;

    {
        bool thrown = false;
        try {
            DefaultRingBuffer invalid(0);
        } catch (const std::invalid_argument&) {
            thrown = true;
        }
        assert(thrown);
    }

    {
        VectorRingBuffer buffer(8);
        assert(buffer.empty());
        assert(!buffer.full());
        assert(buffer.capacity() == 8);
        assert(buffer.readable() == 0);
        assert(buffer.writable() == 8);
        assert(buffer.write(nullptr, 4) == 0);
        assert(buffer.write("abc", 0) == 0);

        char emptyOut[1]{};
        assert(buffer.read(emptyOut, 0) == 0);
        assert(buffer.read(nullptr, 1) == 0);

        std::array<std::span<const std::byte>, 2> emptyReadSpans{};
        assert(buffer.readSpans(emptyReadSpans) == 0);

        assert(buffer.write("abcdef", 6) == 6);
        assert(buffer.readable() == 6);
        assert(buffer.writable() == 2);

        char out[4]{};
        assert(buffer.read(out, sizeof(out)) == 4);
        assert(std::string(out, 4) == "abcd");
        assert(buffer.readable() == 2);

        assert(buffer.write("ghijkl", 6) == 6);
        assert(buffer.full());

        std::array<std::span<const std::byte>, 2> readSpans{};
        const size_t readSpanCount = buffer.readSpans(readSpans);
        assert(readSpanCount == 2);
        assert(readSpans[0].size() == 4);
        assert(readSpans[1].size() == 4);

        char all[8]{};
        assert(buffer.read(all, sizeof(all)) == 8);
        assert(std::string(all, 8) == "efghijkl");
        assert(buffer.empty());
    }

    {
        VectorRingBuffer buffer(4);

        std::array<std::span<std::byte>, 2> writeSpans{};
        const size_t writeSpanCount = buffer.writeSpans(writeSpans);
        assert(writeSpanCount == 1);
        assert(writeSpans[0].size() == 4);

        std::memcpy(writeSpans[0].data(), "wxyz", 4);
        buffer.produce(10);
        assert(buffer.full());
        assert(buffer.readable() == 4);

        buffer.consume(10);
        assert(buffer.empty());
        assert(buffer.writable() == 4);
    }

#if defined(__unix__) || defined(__APPLE__)
    {
        DefaultRingBuffer buffer(8);
        const size_t capacity = buffer.capacity();
        assert(capacity >= 8);

        std::string prefix(capacity - 2, 'x');
        assert(buffer.write(prefix.data(), prefix.size()) == prefix.size());
        buffer.consume(capacity - 4);
        assert(buffer.write("abcdef", 6) == 6);

        std::array<struct iovec, 2> readIovecs{};
        const size_t readCount = buffer.getReadIovecs(readIovecs);
        assert(readCount == 1);
        assert(readIovecs[0].iov_len == buffer.readable());
    }

    {
        VectorRingBuffer buffer(8);

        std::array<struct iovec, 2> writeIovecs{};
        const size_t writeCount = buffer.getWriteIovecs(writeIovecs);
        assert(writeCount == 1);
        assert(writeIovecs[0].iov_len == 8);

        std::memcpy(writeIovecs[0].iov_base, "abcdefgh", 8);
        buffer.produce(6);
        buffer.consume(4);
        assert(buffer.write("ijklmn", 6) == 6);

        std::array<struct iovec, 2> readIovecs{};
        const size_t readCount = buffer.getReadIovecs(readIovecs);
        assert(readCount == 2);
        assert(readIovecs[0].iov_len == 4);
        assert(readIovecs[1].iov_len == 4);

        std::string merged;
        merged.append(static_cast<const char*>(readIovecs[0].iov_base), readIovecs[0].iov_len);
        merged.append(static_cast<const char*>(readIovecs[1].iov_base), readIovecs[1].iov_len);
        assert(merged == "efijklmn");

        assert(buffer.getReadIovecs(nullptr, 2) == 0);
        assert(buffer.getReadIovecs(readIovecs.data(), 0) == 0);
        assert(buffer.getWriteIovecs(nullptr, 2) == 0);
        assert(buffer.getWriteIovecs(writeIovecs.data(), 0) == 0);
        assert(buffer.full());
        assert(buffer.getWriteIovecs(writeIovecs) == 0);
    }
#endif

    {
        VectorRingBuffer buffer(5);
        assert(buffer.write("abcde", 5) == 5);
        assert(buffer.write("z", 1) == 0);

        char out[3]{};
        assert(buffer.read(out, sizeof(out)) == 3);
        assert(std::string(out, 3) == "abc");

        assert(buffer.write("fg", 2) == 2);

        VectorRingBuffer moved(std::move(buffer));
        assert(moved.readable() == 4);
        assert(buffer.empty());
        assert(buffer.readable() == 0);
        char movedOut[4]{};
        assert(moved.read(movedOut, sizeof(movedOut)) == 4);
        assert(std::string(movedOut, 4) == "defg");
        assert(moved.empty());

        VectorRingBuffer assigned(3);
        assert(assigned.write("xy", 2) == 2);
        assigned = std::move(moved);
        assert(assigned.empty());
        assert(moved.empty());
    }

    std::cout << "RingBuffer tests passed!" << std::endl;
}

void test_byte_meta_data_helpers() {
    std::cout << "=== Testing ByteMetaData helpers ===" << std::endl;

    ByteMetaData meta = mallocBytes(8);
    assert(meta.data != nullptr);
    assert(meta.size == 0);
    assert(meta.capacity == 8);

    std::memcpy(meta.data, "abcd", 4);
    meta.size = 4;

    ByteMetaData copy = deepCopyBytes(meta);
    assert(copy.data != nullptr);
    assert(copy.data != meta.data);
    assert(copy.size == 4);
    assert(copy.capacity == 8);
    assert(std::memcmp(copy.data, "abcd", 4) == 0);

    reallocBytes(copy, 2);
    assert(copy.size == 2);
    assert(copy.capacity == 2);
    assert(std::memcmp(copy.data, "ab", 2) == 0);

    clearBytes(copy);
    assert(copy.data != nullptr);
    assert(copy.size == 0);
    assert(copy.capacity == 2);

    freeBytes(copy);
    assert(copy.data == nullptr);
    assert(copy.size == 0);
    assert(copy.capacity == 0);

    freeBytes(meta);

    std::cout << "ByteMetaData helper tests passed!" << std::endl;
}

void test_bytes_container() {
    std::cout << "=== Testing Bytes ===" << std::endl;

    std::string source = "hello";
    Bytes owned(source);
    source[0] = 'H';
    assert(owned.toString() == "hello");
    assert(owned.size() == 5);
    assert(owned.capacity() == 5);
    assert(!owned.empty());

    Bytes literal("hello", 5);
    assert(owned == literal);
    assert(!(owned != literal));

    std::string viewSource = "view";
    Bytes view = Bytes::fromString(viewSource);
    assert(view.toStringView() == "view");
    viewSource[0] = 'V';
    assert(view.toStringView() == "View");

    Bytes raw = Bytes::fromCString("abcdef", 3, 6);
    assert(raw.toString() == "abc");
    assert(raw.capacity() == 6);

    Bytes moved(std::move(owned));
    assert(moved.toString() == "hello");
    assert(owned.empty());
    assert(owned.data() == nullptr);

    Bytes assigned(4);
    assigned = std::move(moved);
    assert(assigned.toString() == "hello");
    assert(moved.empty());

    assigned.clear();
    assert(assigned.empty());
    assert(assigned.data() == nullptr);

    std::cout << "Bytes tests passed!" << std::endl;
}

// ==================== BackTrace Tests ====================

int main() {
    std::cout << "\n=== buffer_test ===" << std::endl;
    try {
        test_buffer_headers_moved_to_cache();
        test_byte_meta_data_helpers();
        test_bytes_container();
        test_byte_queue_view();
        test_ring_buffer();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
