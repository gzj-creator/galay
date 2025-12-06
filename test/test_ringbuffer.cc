#include "galay/common/Buffer.h"
#include <iostream>
#include <cassert>
#include <cstring>

using namespace galay;

void testBasicOperations() {
    std::cout << "测试基本操作..." << std::endl;
    
    RingBuffer rb(100);
    
    // 获取写缓冲区
    auto [write_ptr, write_size] = rb.getWriteBuffer();
    assert(write_ptr != nullptr);
    assert(write_size == 100);
    
    // 写入数据
    std::memcpy(write_ptr, "Hello World", 11);
    rb.produce(11);
    assert(rb.readable() == 11);
    assert(rb.writable() == 89);
    
    // 获取读缓冲区 - 应该一次性返回所有11字节
    auto [read_ptr, read_size] = rb.getReadBuffer();
    assert(read_ptr != nullptr);
    assert(read_size == 11);
    assert(std::memcmp(read_ptr, "Hello World", 11) == 0);
    
    // 消费数据
    rb.consume(11);
    assert(rb.empty());
    
    std::cout << "✓ 基本操作测试通过" << std::endl;
}

void testWrapAroundRead() {
    std::cout << "测试环绕读取（一次性返回所有数据）..." << std::endl;
    
    RingBuffer rb(20);
    
    // 写入15字节
    auto [wptr1, wsize1] = rb.getWriteBuffer();
    std::memcpy(wptr1, "123456789ABCDEF", 15);
    rb.produce(15);
    
    // 消费10字节
    rb.consume(10);
    assert(rb.readable() == 5); // 剩余 "ABCDEF"中的"ABCDE"
    
    // 再写入12字节（会环绕）
    auto [wptr2, wsize2] = rb.getWriteBuffer();
    std::memcpy(wptr2, "GHIJKLMNOPQR", 12);
    rb.produce(12);
    
    // 现在有17字节: "ABCDEGHIJKLMNOPQR"
    assert(rb.readable() == 17);
    
    // 一次性读取所有17字节（即使物理上环绕了）
    auto [rptr, rsize] = rb.getReadBuffer();
    assert(rptr != nullptr);
    assert(rsize == 17);
    
    char expected[] = "ABCDEGHIJKLMNOPQR";
    assert(std::memcmp(rptr, expected, 17) == 0);
    
    rb.consume(17);
    assert(rb.empty());
    
    std::cout << "✓ 环绕读取测试通过" << std::endl;
}

void testMultipleWrites() {
    std::cout << "测试多次写入..." << std::endl;
    
    RingBuffer rb(50);
    
    // 第一次写入
    auto [wptr1, wsize1] = rb.getWriteBuffer();
    std::memcpy(wptr1, "First-", 6);
    rb.produce(6);
    
    // 第二次写入
    auto [wptr2, wsize2] = rb.getWriteBuffer();
    std::memcpy(wptr2, "Second-", 7);
    rb.produce(7);
    
    // 第三次写入
    auto [wptr3, wsize3] = rb.getWriteBuffer();
    std::memcpy(wptr3, "Third", 5);
    rb.produce(5);
    
    // 一次性读取所有18字节
    auto [rptr, rsize] = rb.getReadBuffer();
    assert(rsize == 18);
    assert(std::memcmp(rptr, "First-Second-Third", 18) == 0);
    
    std::cout << "✓ 多次写入测试通过" << std::endl;
}

void testPartialConsume() {
    std::cout << "测试部分消费..." << std::endl;
    
    RingBuffer rb(30);
    
    // 写入数据
    auto [wptr, wsize] = rb.getWriteBuffer();
    std::memcpy(wptr, "0123456789ABCDEFGHIJ", 20);
    rb.produce(20);
    
    // 部分消费
    rb.consume(5);
    assert(rb.readable() == 15);
    
    // 读取剩余数据
    auto [rptr, rsize] = rb.getReadBuffer();
    assert(rsize == 15);
    assert(std::memcmp(rptr, "56789ABCDEFGHIJ", 15) == 0);
    
    // 再部分消费
    rb.consume(10);
    assert(rb.readable() == 5);
    
    auto [rptr2, rsize2] = rb.getReadBuffer();
    assert(rsize2 == 5);
    assert(std::memcmp(rptr2, "FGHIJ", 5) == 0);
    
    std::cout << "✓ 部分消费测试通过" << std::endl;
}

void testResize() {
    std::cout << "测试扩容..." << std::endl;
    
    RingBuffer rb(10);
    
    // 写入8字节
    auto [wptr1, wsize1] = rb.getWriteBuffer();
    std::memcpy(wptr1, "12345678", 8);
    rb.produce(8);
    
    // 扩容到30
    rb.resize(30);
    assert(rb.capacity() == 30);
    assert(rb.readable() == 8);
    assert(rb.writable() == 22);
    
    // 验证数据完整性
    auto [rptr, rsize] = rb.getReadBuffer();
    assert(rsize == 8);
    assert(std::memcmp(rptr, "12345678", 8) == 0);
    
    // 扩容后继续写入
    auto [wptr2, wsize2] = rb.getWriteBuffer();
    std::memcpy(wptr2, "ABCDEFGH", 8);
    rb.produce(8);
    
    // 一次性读取所有16字节
    auto [rptr2, rsize2] = rb.getReadBuffer();
    assert(rsize2 == 16);
    assert(std::memcmp(rptr2, "12345678ABCDEFGH", 16) == 0);
    
    std::cout << "✓ 扩容测试通过" << std::endl;
}

void testResizeWithWrapAround() {
    std::cout << "测试环绕状态下扩容..." << std::endl;
    
    RingBuffer rb(15);
    
    // 写入12字节
    auto [wptr1, wsize1] = rb.getWriteBuffer();
    std::memcpy(wptr1, "123456789ABC", 12);
    rb.produce(12);
    
    // 消费8字节
    rb.consume(8);
    assert(rb.readable() == 4); // 剩余 "9ABC"
    
    // 再写入10字节（会环绕）
    auto [wptr2, wsize2] = rb.getWriteBuffer();
    std::memcpy(wptr2, "DEFGHIJKLM", 10);
    rb.produce(10);
    
    // 此时有14字节，数据环绕了
    assert(rb.readable() == 14);
    
    // 扩容
    rb.resize(30);
    assert(rb.capacity() == 30);
    assert(rb.readable() == 14);
    
    // 验证数据完整性（即使之前环绕了，现在应该是连续的）
    auto [rptr, rsize] = rb.getReadBuffer();
    assert(rsize == 14);
    assert(std::memcmp(rptr, "9ABCDEFGHIJKLM", 14) == 0);
    
    std::cout << "✓ 环绕状态下扩容测试通过" << std::endl;
}

void testComplexScenario() {
    std::cout << "测试复杂场景..." << std::endl;
    
    RingBuffer rb(25);
    
    // 场景：模拟网络数据接收
    // 第一次接收10字节
    auto [wptr1, wsize1] = rb.getWriteBuffer();
    std::memcpy(wptr1, "Request1--", 10);
    rb.produce(10);
    
    // 处理5字节
    rb.consume(5);
    
    // 第二次接收15字节（会环绕）
    auto [wptr2, wsize2] = rb.getWriteBuffer();
    std::memcpy(wptr2, "Request2------3", 15);
    rb.produce(15);
    
    // 现在有20字节数据
    assert(rb.readable() == 20);
    
    // 一次性读取所有数据
    auto [rptr, rsize] = rb.getReadBuffer();
    assert(rsize == 20);
    assert(std::memcmp(rptr, "st1--Request2------3", 20) == 0);
    
    // 处理10字节
    rb.consume(10);
    
    // 再接收数据
    auto [wptr3, wsize3] = rb.getWriteBuffer();
    std::memcpy(wptr3, "End", 3);
    rb.produce(3);
    
    // 读取剩余数据
    auto [rptr2, rsize2] = rb.getReadBuffer();
    assert(rsize2 == 13);
    assert(std::memcmp(rptr2, "uest2------3End", 13) == 0);
    
    std::cout << "✓ 复杂场景测试通过" << std::endl;
}

void testClear() {
    std::cout << "测试清空..." << std::endl;
    
    RingBuffer rb(20);
    
    auto [wptr, wsize] = rb.getWriteBuffer();
    std::memcpy(wptr, "Test Data", 9);
    rb.produce(9);
    
    assert(rb.readable() == 9);
    
    rb.clear();
    assert(rb.empty());
    assert(rb.readable() == 0);
    assert(rb.writable() == 20);
    
    // 清空后可以继续使用
    auto [wptr2, wsize2] = rb.getWriteBuffer();
    assert(wsize2 == 20);
    
    std::cout << "✓ 清空测试通过" << std::endl;
}

void testMoveSemantics() {
    std::cout << "测试移动语义..." << std::endl;
    
    RingBuffer rb1(50);
    auto [wptr, wsize] = rb1.getWriteBuffer();
    std::memcpy(wptr, "Move Test Data", 14);
    rb1.produce(14);
    
    // 移动构造
    RingBuffer rb2(std::move(rb1));
    assert(rb2.readable() == 14);
    assert(rb1.capacity() == 0);
    
    // 移动赋值
    RingBuffer rb3(30);
    rb3 = std::move(rb2);
    assert(rb3.readable() == 14);
    
    auto [rptr, rsize] = rb3.getReadBuffer();
    assert(std::memcmp(rptr, "Move Test Data", 14) == 0);
    
    std::cout << "✓ 移动语义测试通过" << std::endl;
}

int main() {
    std::cout << "开始RingBuffer测试..." << std::endl << std::endl;
    
    try {
        testBasicOperations();
        testWrapAroundRead();
        testMultipleWrites();
        testPartialConsume();
        testResize();
        testResizeWithWrapAround();
        testComplexScenario();
        testClear();
        testMoveSemantics();
        
        std::cout << std::endl << "✓ 所有测试通过！" << std::endl;
        std::cout << std::endl << "RingBuffer特性：" << std::endl;
        std::cout << "  1. getWriteBuffer() 返回写指针和可写大小" << std::endl;
        std::cout << "  2. getReadBuffer() 返回读指针和可读大小（自动整理，保证数据连续）" << std::endl;
        std::cout << "  3. produce(n) 提交写入n字节" << std::endl;
        std::cout << "  4. consume(n) 消费n字节" << std::endl;
        std::cout << "  5. resize(size) 扩容到指定大小" << std::endl;
        std::cout << "  6. 即使数据物理上环绕，getReadBuffer()也会返回连续的逻辑视图" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "✗ 测试失败: " << e.what() << std::endl;
        return 1;
    }
}
