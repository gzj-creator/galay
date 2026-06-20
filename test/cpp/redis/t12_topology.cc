#include <iostream>
#include <string>
#include <vector>

#include <galay/cpp/galay-redis/async/topology_client.h>

using namespace galay::kernel;
using namespace galay::redis;

namespace
{
    class CountingHoldScheduler final : public IOScheduler
    {
    public:
        void start() override {}
        void stop() override {}

        bool schedule(TaskRef task) override
        {
            if (!bindTask(task)) {
                return false;
            }
            ++m_spawn_count;
            m_pending.push_back(std::move(task));
            return true;
        }

        bool scheduleDeferred(TaskRef task) override
        {
            return schedule(std::move(task));
        }

        bool scheduleImmediately(TaskRef task) override
        {
            if (!bindTask(task)) {
                return false;
            }
            ++m_spawn_count;
            resume(task);
            return true;
        }

        int addAccept(IOController*) override { return -1; }
        int addConnect(IOController*) override { return -1; }
        int addRecv(IOController*) override { return -1; }
        int addSend(IOController*) override { return -1; }
        int addReadv(IOController*) override { return -1; }
        int addWritev(IOController*) override { return -1; }
        int addClose(IOController*) override { return -1; }
        int addFileRead(IOController*) override { return -1; }
        int addFileWrite(IOController*) override { return -1; }
        int addRecvFrom(IOController*) override { return -1; }
        int addSendTo(IOController*) override { return -1; }
        int addFileWatch(IOController*) override { return -1; }
        int addSendFile(IOController*) override { return -1; }
        int addSequence(IOController*) override { return -1; }
        int remove(IOController*) override { return -1; }

        size_t spawnCount() const noexcept { return m_spawn_count; }

        void drain()
        {
            while (!m_pending.empty()) {
                auto task = std::move(m_pending.back());
                m_pending.pop_back();
                resume(task);
            }
        }

    private:
        size_t m_spawn_count = 0;
        std::vector<TaskRef> m_pending;
    };

    int g_failures = 0;

    void expectEqual(const std::string& name, size_t actual, size_t expected)
    {
        if (actual != expected) {
            ++g_failures;
            std::cerr << "[FAILED] " << name << ": expected=" << expected << ", actual=" << actual << std::endl;
        }
    }
}

int main()
{
    std::cout << "Running topology task laziness tests..." << std::endl;

    {
        CountingHoldScheduler scheduler;
        auto ms = RedisMasterSlaveClientBuilder().scheduler(&scheduler).build();

        auto refresh1 = ms.refreshFromSentinel();
        auto refresh2 = ms.refreshFromSentinel();
        auto refresh3 = ms.refreshFromSentinel();
        expectEqual("refreshFromSentinel task creation should be lazy", scheduler.spawnCount(), 0);

        scheduleTask(&scheduler, std::move(refresh1));
        scheduleTask(&scheduler, std::move(refresh2));
        scheduleTask(&scheduler, std::move(refresh3));
        expectEqual("refreshFromSentinel scheduled task count", scheduler.spawnCount(), 3);
        scheduler.drain();
    }

    {
        CountingHoldScheduler scheduler;
        auto cluster = RedisClusterClientBuilder().scheduler(&scheduler).build();

        auto refresh1 = cluster.refreshSlots();
        auto refresh2 = cluster.refreshSlots();
        auto refresh3 = cluster.refreshSlots();
        expectEqual("refreshSlots task creation should be lazy", scheduler.spawnCount(), 0);

        scheduleTask(&scheduler, std::move(refresh1));
        scheduleTask(&scheduler, std::move(refresh2));
        scheduleTask(&scheduler, std::move(refresh3));
        expectEqual("refreshSlots scheduled task count", scheduler.spawnCount(), 3);
        scheduler.drain();
    }

    if (g_failures != 0) {
        std::cerr << "[FAILED] topology task laziness tests failed, count=" << g_failures << std::endl;
        return 1;
    }

    std::cout << "[PASSED] topology task laziness tests passed" << std::endl;
    return 0;
}
