/**
 * @file t87_seqsync.cc
 * @brief 用途：验证 sequence interest 会按当前 active task 和 split owner 正确收敛到 controller cache。
 * 关键覆盖点：controller 级 sequence interest helper、read/write slot mask、split owner 合并。
 * 通过条件：interest 收集与同步结果都符合预期，测试返回 0。
 */

#include "galay-kernel/core/awaitable.h"

#include <expected>
#include <iostream>

using namespace galay::kernel;

namespace {

struct StubSequenceOwner final : SequenceAwaitableBase {
    explicit StubSequenceOwner(IOController* controller,
                               SequenceOwnerDomain domain = SequenceOwnerDomain::ReadWrite)
        : SequenceAwaitableBase(controller, domain) {}

    void setActive(IOEventType type) {
        m_active = type;
        m_task.type = type;
        m_task.task = nullptr;
        m_task.context = nullptr;
    }

    IOTask* front() override {
        return m_active == IOEventType::INVALID ? nullptr : &m_task;
    }

    const IOTask* front() const override {
        return m_active == IOEventType::INVALID ? nullptr : &m_task;
    }

    void popFront() override {
        m_active = IOEventType::INVALID;
    }

    bool empty() const override {
        return m_active == IOEventType::INVALID;
    }

#ifdef USE_IOURING
    SequenceProgress prepareForSubmit() override {
        return SequenceProgress::kNeedWait;
    }

    SequenceProgress onActiveEvent(struct io_uring_cqe*, GHandle) override {
        return SequenceProgress::kNeedWait;
    }
#else
    SequenceProgress prepareForSubmit(GHandle) override {
        return SequenceProgress::kNeedWait;
    }

    SequenceProgress onActiveEvent(GHandle) override {
        return SequenceProgress::kNeedWait;
    }
#endif

    IOEventType m_active = IOEventType::INVALID;
    IOTask m_task{};
};

bool verifySingleOwnerInterestFollowsActiveTask() {
    IOController controller(GHandle::invalid());
    StubSequenceOwner owner(&controller, SequenceOwnerDomain::ReadWrite);
    controller.m_sequence_owner[IOController::READ] = &owner;
    controller.m_sequence_owner[IOController::WRITE] = &owner;

    owner.setActive(RECV);
    const auto read_mask = detail::syncSequenceInterestMask(&controller);
    if (read_mask != detail::sequenceSlotMask(IOController::READ) ||
        controller.m_sequence_interest_mask != read_mask) {
        std::cerr << "[T87] read active task should produce read interest only\n";
        return false;
    }

    owner.setActive(SEND);
    const auto write_mask = detail::syncSequenceInterestMask(&controller);
    if (write_mask != detail::sequenceSlotMask(IOController::WRITE) ||
        controller.m_sequence_interest_mask != write_mask) {
        std::cerr << "[T87] write active task should produce write interest only\n";
        return false;
    }

    owner.setActive(IOEventType::INVALID);
    const auto empty_mask = detail::syncSequenceInterestMask(&controller);
    if (empty_mask != 0 || controller.m_sequence_interest_mask != 0) {
        std::cerr << "[T87] invalid active task should clear controller interest\n";
        return false;
    }

    return true;
}

bool verifySplitOwnersMergeReadAndWriteInterest() {
    IOController controller(GHandle::invalid());
    StubSequenceOwner read_owner(&controller, SequenceOwnerDomain::Read);
    StubSequenceOwner write_owner(&controller, SequenceOwnerDomain::Write);

    read_owner.setActive(RECV);
    write_owner.setActive(WRITEV);
    controller.m_sequence_owner[IOController::READ] = &read_owner;
    controller.m_sequence_owner[IOController::WRITE] = &write_owner;

    const auto mask = detail::collectSequenceInterestMask(&controller);
    const auto expected = static_cast<uint8_t>(
        detail::sequenceSlotMask(IOController::READ) |
        detail::sequenceSlotMask(IOController::WRITE));
    if (mask != expected) {
        std::cerr << "[T87] split owners should merge read and write interest\n";
        return false;
    }

    if (detail::syncSequenceInterestMask(&controller) != expected ||
        controller.m_sequence_interest_mask != expected) {
        std::cerr << "[T87] sync should persist merged split-owner interest\n";
        return false;
    }

    detail::clearSequenceInterestMask(&controller);
    if (controller.m_sequence_interest_mask != 0 || controller.m_sequence_armed_mask != 0) {
        std::cerr << "[T87] clear helper should reset cached sequence state\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!verifySingleOwnerInterestFollowsActiveTask()) {
        return 1;
    }

    if (!verifySplitOwnersMergeReadAndWriteInterest()) {
        return 1;
    }

    std::cout << "T87-SequenceInterestSync PASS\n";
    return 0;
}
