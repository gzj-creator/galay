#include "io_ready_queue.hpp"

#if defined(USE_IOURING)
#include "uring_scheduler.h"
#elif defined(USE_KQUEUE)
#include "kqueue_scheduler.h"
#elif defined(USE_EPOLL)
#include "epoll_scheduler.h"
#endif

namespace galay::kernel {

bool IOReadyQueue::try_steal() {
    if (!stealing_enabled || has_local_work() || has_pending_injected() || siblings.size() <= 1) {
        return false;
    }

    const size_t local_capacity = local_ring.remaining_capacity();
    if (local_capacity == 0) {
        return false;
    }

    ++steal_attempts;
    std::uniform_int_distribution<size_t> start_dist(0, siblings.size() - 1);
    const size_t start = start_dist(random_seed);

    for (size_t probe = 0; probe < siblings.size(); ++probe) {
        const size_t victim_index = (start + probe) % siblings.size();
        if (victim_index == self_index) {
            continue;
        }

        IOScheduler* const victim_scheduler = siblings[victim_index];
        if (victim_scheduler == nullptr) {
            continue;
        }

        IOReadyQueue* const victim = victim_scheduler->steal_worker_state();
        if (victim == nullptr) {
            continue;
        }

        size_t stolen = 0;
        const size_t victim_size = victim->local_ring.size();
        if (victim_size > 0) {
            const size_t steal_target =
                std::min(local_capacity, std::max<size_t>(1, victim_size / 2));
            for (; stolen < steal_target; ++stolen) {
                detail::ReadyEntry entry;
                if (!victim->steal_front(entry)) {
                    break;
                }
                if (!local_ring.push_back(entry)) {
                    if (!detail::schedule_ready_entry(entry) && entry.is_valid()) {
                        victim->fallback_to_inject(entry);
                    }
                    break;
                }
            }
        } else if (victim->has_owner_drained_injected() && victim->has_pending_injected()) {
            size_t attempts = 0;
            while (stolen < local_capacity && attempts < local_capacity) {
                ++attempts;
                detail::ReadyEntry entry;
                if (!victim->ready_inject_queue.try_dequeue(entry)) {
                    break;
                }
                victim->injected_outstanding.fetch_sub(1, std::memory_order_acq_rel);
                if (!entry.is_valid()) {
                    continue;
                }
                if (detail::ready_entry_resume_owner_only(entry)) {
                    if (!detail::schedule_ready_entry(entry) && entry.is_valid()) {
                        victim->fallback_to_inject(entry);
                    }
                    continue;
                }
                if (!local_ring.push_back(entry)) {
                    if (!detail::schedule_ready_entry(entry) && entry.is_valid()) {
                        victim->fallback_to_inject(entry);
                    }
                    break;
                }
                ++stolen;
            }
        }

        if (stolen == 0) {
            continue;
        }

        if (stolen > 0) {
            lifo_enabled = true;
            consecutive_lifo_polls = 0;
            polls_since_inject = 0;
            ++steal_successes;
            return true;
        }
    }

    return false;
}


} // namespace galay::kernel
