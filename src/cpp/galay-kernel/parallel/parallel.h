/**
 * @file parallel.h
 * @brief Structured parallel execution for synchronous CPU work.
 *
 * The first version deliberately keeps the node surface small: a graph node is
 * a non-suspending callable returning `void` or
 * `std::expected<void, ParallelError>`. Nodes are submitted as ParallelWorkItem,
 * not as Task<void>, so a graph does not allocate one coroutine frame per node.
 */

#ifndef GALAY_KERNEL_PARALLEL_H
#define GALAY_KERNEL_PARALLEL_H

#include "parallel_scheduler.h"
#include "../core/runtime.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <coroutine>
#include <deque>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace galay::kernel
{

enum class ParallelErrorCode : std::uint8_t {
    kNone,
    kInvalidGraph,
    kCycleDetected,
    kInvalidNode,
    kDuplicateDependency,
    kAllocationFailed,
    kSchedulerUnavailable,
    kScheduleFailed,
    kWorkFailed,
    kTaskFailed,
    kResumeFailed,
    kSkipped,
};

/**
 * @brief Error returned by a parallel graph.
 * @details `node()` identifies the first node that reported a non-skipped error
 * when one is available. `kSkipped` is an internal propagation marker and is
 * not selected as the graph's first error.
 */
class ParallelError
{
public:
    static constexpr std::size_t kNoNode = std::numeric_limits<std::size_t>::max();

    constexpr ParallelError() noexcept = default;

    constexpr explicit ParallelError(ParallelErrorCode code,
                                     std::size_t node = kNoNode) noexcept
        : m_code(code),
          m_node(node)
    {
    }

    constexpr ParallelErrorCode code() const noexcept { return m_code; }
    constexpr std::size_t node() const noexcept { return m_node; }
    constexpr bool hasError() const noexcept { return m_code != ParallelErrorCode::kNone; }

    std::string_view message() const noexcept
    {
        switch (m_code) {
        case ParallelErrorCode::kNone:
            return "parallel execution completed";
        case ParallelErrorCode::kInvalidGraph:
            return "parallel graph is invalid";
        case ParallelErrorCode::kCycleDetected:
            return "parallel graph contains a cycle";
        case ParallelErrorCode::kInvalidNode:
            return "parallel graph node is invalid";
        case ParallelErrorCode::kDuplicateDependency:
            return "parallel graph dependency already exists";
        case ParallelErrorCode::kAllocationFailed:
            return "parallel execution allocation failed";
        case ParallelErrorCode::kSchedulerUnavailable:
            return "parallel execution has no parallel scheduler";
        case ParallelErrorCode::kScheduleFailed:
            return "parallel work could not be scheduled";
        case ParallelErrorCode::kWorkFailed:
            return "parallel work returned an error";
        case ParallelErrorCode::kTaskFailed:
            return "parallel task returned an error";
        case ParallelErrorCode::kResumeFailed:
            return "parallel parent task could not be resumed";
        case ParallelErrorCode::kSkipped:
            return "parallel work was skipped after a predecessor failed";
        }
        return "unknown parallel error";
    }

private:
    ParallelErrorCode m_code = ParallelErrorCode::kNone;
    std::size_t m_node = kNoNode;
};

namespace detail
{

class ParallelState;
class ParallelAwaitable;

} // namespace detail

/**
 * @brief Type-erased synchronous work item used by a parallel graph.
 * @details The callable is owned by this object and is invoked at most once.
 * It must be nothrow-invocable and return `void` or
 * `std::expected<void, ParallelError>`.
 */
class ParallelWork
{
public:
    using Invoke = ParallelError (*)(void* object) noexcept;
    using Destroy = void (*)(void* object) noexcept;

    constexpr ParallelWork() noexcept = default;

    ParallelWork(const ParallelWork&) = delete;
    ParallelWork& operator=(const ParallelWork&) = delete;

    ParallelWork(ParallelWork&& other) noexcept
        : m_object(other.m_object),
          m_invoke(other.m_invoke),
          m_destroy(other.m_destroy)
    {
        other.clear();
    }

    ParallelWork& operator=(ParallelWork&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_object = other.m_object;
            m_invoke = other.m_invoke;
            m_destroy = other.m_destroy;
            other.clear();
        }
        return *this;
    }

    ~ParallelWork() { reset(); }

    bool valid() const noexcept
    {
        return m_object != nullptr && m_invoke != nullptr && m_destroy != nullptr;
    }

    ParallelError invoke() const noexcept
    {
        if (!valid()) {
            return ParallelError(ParallelErrorCode::kAllocationFailed);
        }
        return m_invoke(m_object);
    }

    void reset() noexcept
    {
        if (m_object != nullptr && m_destroy != nullptr) {
            m_destroy(m_object);
        }
        clear();
    }

    template <typename F>
    static ParallelWork make(F&& function) noexcept
    {
        using Fn = std::decay_t<F>;
        using Result = std::invoke_result_t<Fn&>;
        static_assert(std::is_same_v<Result, void> ||
                          std::is_same_v<Result, std::expected<void, ParallelError>>,
                      "parallel::work requires void or expected<void, ParallelError>");

        struct Model {
            explicit Model(Fn&& value) noexcept(std::is_nothrow_move_constructible_v<Fn>)
                : function(std::move(value))
            {
            }

            Fn function;
        };

        static_assert(std::is_nothrow_move_constructible_v<Fn>,
                      "parallel::work callable must be nothrow movable");
        static_assert(std::is_nothrow_constructible_v<Fn, F&&>,
                      "parallel::work callable construction must be noexcept");
        static_assert(std::is_nothrow_invocable_v<Fn&>,
                      "parallel::work callable must be noexcept");

        auto* model = new (std::nothrow) Model(Fn(std::forward<F>(function)));
        if (model == nullptr) {
            return {};
        }

        ParallelWork result;
        result.m_object = model;
        result.m_invoke = [](void* object) noexcept {
            auto& callable = static_cast<Model*>(object)->function;
            if constexpr (std::is_same_v<Result, void>) {
                std::invoke(callable);
                return ParallelError{};
            } else {
                auto outcome = std::invoke(callable);
                if (outcome.has_value()) {
                    return ParallelError{};
                }
                return outcome.error();
            }
        };
        result.m_destroy = [](void* object) noexcept {
            delete static_cast<Model*>(object);
        };
        return result;
    }

private:
    friend class detail::ParallelState;

    void clear() noexcept
    {
        m_object = nullptr;
        m_invoke = nullptr;
        m_destroy = nullptr;
    }

    void* m_object = nullptr;
    Invoke m_invoke = nullptr;
    Destroy m_destroy = nullptr;
};

/**
 * @brief Wrap a non-suspending CPU callable without creating a coroutine.
 * @note The callable must be `noexcept` and return `void` or
 * `std::expected<void, ParallelError>`.
 */
template <typename F>
ParallelWork makeParallelWork(F&& function) noexcept
{
    return ParallelWork::make(std::forward<F>(function));
}

using ParallelNodeId = std::size_t;

/**
 * @brief A closed-over-time DAG of synchronous CPU work.
 *
 * Nodes are added before `parallel(std::move(graph))` is awaited. `then(a, b)`
 * adds an edge from `a` to `b`; `b` is submitted only after every predecessor
 * has completed successfully. A failed predecessor skips its pending
 * descendants. The graph result is published only after every node is
 * terminal, including rejected and skipped nodes.
 */
class ParallelGraph
{
public:
    static constexpr ParallelNodeId kInvalidNode = ParallelError::kNoNode;

    ParallelGraph() = default;
    ParallelGraph(const ParallelGraph&) = delete;
    ParallelGraph& operator=(const ParallelGraph&) = delete;
    ParallelGraph(ParallelGraph&&) noexcept = default;
    ParallelGraph& operator=(ParallelGraph&&) noexcept = default;

    std::expected<ParallelNodeId, ParallelError> add(ParallelWork work)
    {
        if (m_sealed) {
            return std::unexpected(ParallelError(ParallelErrorCode::kInvalidGraph));
        }
        if (!work.valid()) {
            return std::unexpected(ParallelError(ParallelErrorCode::kAllocationFailed));
        }

        const auto id = m_nodes.size();
        m_nodes.emplace_back(std::move(work));
        return id;
    }

    template <typename F>
    std::expected<ParallelNodeId, ParallelError> add(F&& function)
    {
        return add(makeParallelWork(std::forward<F>(function)));
    }

    std::expected<void, ParallelError> then(ParallelNodeId predecessor,
                                            ParallelNodeId successor)
    {
        if (m_sealed) {
            return std::unexpected(ParallelError(ParallelErrorCode::kInvalidGraph));
        }
        if (!validNode(predecessor) || !validNode(successor) || predecessor == successor) {
            return std::unexpected(ParallelError(ParallelErrorCode::kInvalidNode));
        }

        auto& successors = m_nodes[predecessor].successors;
        if (std::find(successors.begin(), successors.end(), successor) != successors.end()) {
            return std::unexpected(ParallelError(ParallelErrorCode::kDuplicateDependency));
        }

        successors.push_back(successor);
        ++m_nodes[successor].predecessor_count;
        return {};
    }

    std::size_t size() const noexcept { return m_nodes.size(); }
    bool empty() const noexcept { return m_nodes.empty(); }

private:
    enum class NodeState : std::uint8_t {
        kPending,
        kRunning,
        kSucceeded,
        kFailed,
        kSkipped,
        kRejected,
    };

    struct Node {
        explicit Node(ParallelWork value) noexcept
            : work(std::move(value))
        {
        }

        ParallelWork work;
        std::vector<ParallelNodeId> successors;
        std::size_t predecessor_count = 0;
        std::atomic<std::size_t> pending_predecessors{0};
        std::atomic<NodeState> state{NodeState::kPending};
        ParallelError error{};
    };

    friend class detail::ParallelState;
    friend class detail::ParallelAwaitable;

    bool validNode(ParallelNodeId id) const noexcept
    {
        return id < m_nodes.size();
    }

    std::expected<void, ParallelError> validate() const
    {
        if (m_nodes.empty()) {
            return {};
        }

        std::vector<std::size_t> indegrees;
        indegrees.reserve(m_nodes.size());
        std::deque<ParallelNodeId> ready;
        for (const auto& node : m_nodes) {
            indegrees.push_back(node.predecessor_count);
        }
        for (ParallelNodeId id = 0; id < indegrees.size(); ++id) {
            if (indegrees[id] == 0) {
                ready.push_back(id);
            }
        }

        std::size_t visited = 0;
        while (!ready.empty()) {
            const auto id = ready.front();
            ready.pop_front();
            ++visited;
            for (const auto successor : m_nodes[id].successors) {
                if (--indegrees[successor] == 0) {
                    ready.push_back(successor);
                }
            }
        }

        if (visited != m_nodes.size()) {
            return std::unexpected(ParallelError(ParallelErrorCode::kCycleDetected));
        }
        return {};
    }

    std::deque<Node> m_nodes;
    bool m_sealed = false;
};

namespace detail
{

class ParallelState
{
public:
    ParallelState(ParallelGraph&& graph,
                  TaskRef parent,
                  Runtime* runtime,
                  Scheduler* parent_scheduler) noexcept
        : m_graph(std::move(graph)),
          m_parent(std::move(parent)),
          m_runtime(runtime),
          m_parent_scheduler(parent_scheduler),
          m_remaining(m_graph.size())
    {
        m_graph.m_sealed = true;
        for (auto& node : m_graph.m_nodes) {
            node.pending_predecessors.store(node.predecessor_count,
                                            std::memory_order_relaxed);
        }
    }

    ParallelState(const ParallelState&) = delete;
    ParallelState& operator=(const ParallelState&) = delete;

    void retain() noexcept
    {
        m_refs.fetch_add(1, std::memory_order_relaxed);
    }

    void release() noexcept
    {
        if (m_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

    void start() noexcept
    {
        for (ParallelNodeId id = 0; id < m_graph.m_nodes.size(); ++id) {
            if (m_graph.m_nodes[id].predecessor_count == 0) {
                submitNode(id);
            }
        }
    }

    bool completed() const noexcept
    {
        return m_completed.load(std::memory_order_acquire);
    }

    bool resumeFailed() const noexcept
    {
        return m_resume_failed.load(std::memory_order_acquire);
    }

    std::expected<void, ParallelError> result() const noexcept
    {
        if (resumeFailed()) {
            return std::unexpected(ParallelError(ParallelErrorCode::kResumeFailed));
        }

        const auto error_node = m_first_error_node.load(std::memory_order_acquire);
        if (error_node == ParallelError::kNoNode) {
            return {};
        }

        const auto& error = m_graph.m_nodes[error_node].error;
        if (error.node() != ParallelError::kNoNode) {
            return std::unexpected(error);
        }
        return std::unexpected(ParallelError(error.code(), error_node));
    }

private:
    static void runWork(void* context, std::size_t index) noexcept
    {
        static_cast<ParallelState*>(context)->runNode(index);
    }

    static void releaseWork(void* context) noexcept
    {
        static_cast<ParallelState*>(context)->release();
    }

    ParallelWorkItem makeWork(ParallelNodeId id) noexcept
    {
        retain();
        return ParallelWorkItem(this, id, &runWork, &releaseWork);
    }

    ParallelScheduler* chooseScheduler() noexcept
    {
        if (m_runtime != nullptr) {
            if (auto* scheduler = m_runtime->getNextParallelScheduler(); scheduler != nullptr) {
                return scheduler;
            }
        }

        if (m_parent_scheduler != nullptr) {
            return dynamic_cast<ParallelScheduler*>(m_parent_scheduler);
        }
        return nullptr;
    }

    void submitNode(ParallelNodeId id) noexcept
    {
        auto& node = m_graph.m_nodes[id];
        auto expected = ParallelGraph::NodeState::kPending;
        if (!node.state.compare_exchange_strong(expected,
                                                ParallelGraph::NodeState::kRunning,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            return;
        }

        auto* scheduler = chooseScheduler();
        if (scheduler == nullptr) {
            terminalizeRunning(id,
                               ParallelGraph::NodeState::kRejected,
                               ParallelError(ParallelErrorCode::kSchedulerUnavailable, id));
            return;
        }

        auto work = makeWork(id);
        if (!scheduler->scheduleWork(std::move(work))) {
            terminalizeRunning(id,
                               ParallelGraph::NodeState::kRejected,
                               ParallelError(ParallelErrorCode::kScheduleFailed, id));
        }
    }

    void runNode(ParallelNodeId id) noexcept
    {
        auto& node = m_graph.m_nodes[id];
        const auto error = node.work.invoke();
        if (error.hasError()) {
            terminalizeRunning(id,
                               ParallelGraph::NodeState::kFailed,
                               error.code() == ParallelErrorCode::kNone
                                   ? ParallelError(ParallelErrorCode::kWorkFailed, id)
                                   : ParallelError(error.code(), id));
            return;
        }
        terminalizeRunning(id, ParallelGraph::NodeState::kSucceeded, {});
    }

    void terminalizeRunning(ParallelNodeId id,
                            ParallelGraph::NodeState terminal_state,
                            ParallelError error) noexcept
    {
        auto& node = m_graph.m_nodes[id];
        auto expected = ParallelGraph::NodeState::kRunning;
        if (!node.state.compare_exchange_strong(expected,
                                                terminal_state,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            return;
        }

        node.error = error;
        node.state.store(terminal_state, std::memory_order_release);
        if (error.hasError() && error.code() != ParallelErrorCode::kSkipped) {
            recordError(id);
        }
        finishNode(id, terminal_state == ParallelGraph::NodeState::kSucceeded);
    }

    void skipNode(ParallelNodeId id, ParallelError cause) noexcept
    {
        auto& node = m_graph.m_nodes[id];
        auto expected = ParallelGraph::NodeState::kPending;
        if (!node.state.compare_exchange_strong(expected,
                                                ParallelGraph::NodeState::kSkipped,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
            return;
        }

        node.error = ParallelError(ParallelErrorCode::kSkipped, cause.node());
        node.state.store(ParallelGraph::NodeState::kSkipped, std::memory_order_release);
        finishNode(id, false);
    }

    void finishNode(ParallelNodeId id, bool succeeded) noexcept
    {
        auto& node = m_graph.m_nodes[id];
        if (succeeded) {
            for (const auto successor : node.successors) {
                auto& next = m_graph.m_nodes[successor];
                const auto previous = next.pending_predecessors.fetch_sub(
                    1, std::memory_order_acq_rel);
                if (previous == 1) {
                    submitNode(successor);
                }
            }
        } else {
            const auto cause = node.error;
            for (const auto successor : node.successors) {
                skipNode(successor, cause);
            }
        }

        if (m_remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            m_completed.store(true, std::memory_order_release);
            if (!requestTaskResume(m_parent)) {
                m_resume_failed.store(true, std::memory_order_release);
            }
        }
    }

    void recordError(ParallelNodeId id) noexcept
    {
        auto expected = ParallelError::kNoNode;
        (void)m_first_error_node.compare_exchange_strong(expected,
                                                          id,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire);
    }

    ParallelGraph m_graph;
    TaskRef m_parent;
    Runtime* m_runtime = nullptr;
    Scheduler* m_parent_scheduler = nullptr;
    std::atomic<std::size_t> m_refs{1};
    std::atomic<std::size_t> m_remaining{0};
    std::atomic<std::size_t> m_first_error_node{ParallelError::kNoNode};
    std::atomic<bool> m_completed{false};
    std::atomic<bool> m_resume_failed{false};
};

class ParallelAwaitable
{
public:
    explicit ParallelAwaitable(ParallelGraph graph) noexcept
        : m_graph(std::move(graph))
    {
    }

    ParallelAwaitable(const ParallelAwaitable&) = delete;
    ParallelAwaitable& operator=(const ParallelAwaitable&) = delete;

    ParallelAwaitable(ParallelAwaitable&& other) noexcept
        : m_graph(std::move(other.m_graph)),
          m_state(other.m_state),
          m_inline_error(other.m_inline_error)
    {
        other.m_state = nullptr;
        other.m_inline_error = {};
    }

    ParallelAwaitable& operator=(ParallelAwaitable&& other) noexcept
    {
        if (this != &other) {
            releaseState();
            m_graph = std::move(other.m_graph);
            m_state = other.m_state;
            m_inline_error = other.m_inline_error;
            other.m_state = nullptr;
            other.m_inline_error = {};
        }
        return *this;
    }

    ~ParallelAwaitable() { releaseState(); }

    bool await_ready() const noexcept
    {
        return m_graph.empty();
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept
    {
        const auto valid = m_graph.validate();
        if (!valid.has_value()) {
            m_inline_error = valid.error();
            return false;
        }

        const TaskRef parent = handle.promise().taskRefView();
        if (!parent.isValid() || parent.belongScheduler() == nullptr) {
            m_inline_error = ParallelError(ParallelErrorCode::kSchedulerUnavailable);
            return false;
        }

        if (m_graph.m_nodes.size() == 1 &&
            parent.belongScheduler()->type() == kParallelScheduler) {
            const auto error = m_graph.m_nodes.front().work.invoke();
            if (error.hasError()) {
                m_inline_error = ParallelError(error.code(), 0);
            }
            return false;
        }

        auto* state = new (std::nothrow) ParallelState(
            std::move(m_graph),
            parent,
            detail::taskRuntime(parent),
            parent.belongScheduler());
        if (state == nullptr) {
            m_inline_error = ParallelError(ParallelErrorCode::kAllocationFailed);
            return false;
        }
        m_state = state;
        m_state->start();
        return true;
    }

    std::expected<void, ParallelError> await_resume() const noexcept
    {
        if (m_state == nullptr) {
            if (m_inline_error.hasError()) {
                return std::unexpected(m_inline_error);
            }
            return {};
        }
        if (!m_state->completed()) {
            return std::unexpected(ParallelError(ParallelErrorCode::kResumeFailed));
        }
        return m_state->result();
    }

private:
    void releaseState() noexcept
    {
        if (m_state != nullptr) {
            m_state->release();
            m_state = nullptr;
        }
    }

    ParallelGraph m_graph;
    ParallelState* m_state = nullptr;
    ParallelError m_inline_error{};
};

} // namespace detail

/**
 * @brief Await completion of a dependency graph on Runtime parallel schedulers.
 * @details The parent coroutine is resumed exactly once, and only after all
 * graph nodes are terminal (succeeded, failed, skipped, or rejected).
 */
inline detail::ParallelAwaitable parallel(ParallelGraph graph) noexcept
{
    return detail::ParallelAwaitable(std::move(graph));
}

} // namespace galay::kernel

#endif // GALAY_KERNEL_PARALLEL_H
