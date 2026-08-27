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
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

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

/**
 * @brief Fixed-element, growable storage whose growth never throws.
 * @details The graph only stores pointers and node IDs, so a trivially-copyable
 *          buffer is sufficient. Capacity failures are reported by `push_back`
 *          instead of escaping an `expected`-returning graph API.
 */
template <typename T>
class NoThrowVector
{
    static_assert(std::is_trivially_copyable_v<T>);

public:
    NoThrowVector() noexcept = default;
    NoThrowVector(const NoThrowVector&) = delete;
    NoThrowVector& operator=(const NoThrowVector&) = delete;

    NoThrowVector(NoThrowVector&& other) noexcept
        : m_data(std::move(other.m_data)),
          m_size(other.m_size),
          m_capacity(other.m_capacity)
    {
        other.m_size = 0;
        other.m_capacity = 0;
    }

    NoThrowVector& operator=(NoThrowVector&& other) noexcept
    {
        if (this != &other) {
            m_data = std::move(other.m_data);
            m_size = other.m_size;
            m_capacity = other.m_capacity;
            other.m_size = 0;
            other.m_capacity = 0;
        }
        return *this;
    }

    bool push_back(const T& value) noexcept
    {
        if (m_size == m_capacity && !grow()) {
            return false;
        }
        m_data[m_size++] = value;
        return true;
    }

    std::size_t size() const noexcept { return m_size; }
    bool empty() const noexcept { return m_size == 0; }

    T& operator[](std::size_t index) noexcept { return m_data[index]; }
    const T& operator[](std::size_t index) const noexcept { return m_data[index]; }

    T* begin() noexcept { return m_data ? m_data.get() : nullptr; }
    T* end() noexcept { return m_data ? m_data.get() + m_size : nullptr; }
    const T* begin() const noexcept { return m_data ? m_data.get() : nullptr; }
    const T* end() const noexcept { return m_data ? m_data.get() + m_size : nullptr; }

private:
    bool grow() noexcept
    {
        const auto max = std::numeric_limits<std::size_t>::max();
        const auto next_capacity = m_capacity == 0
            ? std::size_t{4}
            : (m_capacity > max / 2 ? max : m_capacity * 2);
        if (next_capacity <= m_capacity || next_capacity > max / sizeof(T)) {
            return false;
        }

        std::unique_ptr<T[]> next(new (std::nothrow) T[next_capacity]);
        if (!next) {
            return false;
        }
        if (m_size != 0) {
            std::copy_n(m_data.get(), m_size, next.get());
        }
        m_data = std::move(next);
        m_capacity = next_capacity;
        return true;
    }

    std::unique_ptr<T[]> m_data;
    std::size_t m_size = 0;
    std::size_t m_capacity = 0;
};

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
    ~ParallelGraph() { clear(); }
    ParallelGraph(const ParallelGraph&) = delete;
    ParallelGraph& operator=(const ParallelGraph&) = delete;
    ParallelGraph(ParallelGraph&& other) noexcept
        : m_nodes(std::move(other.m_nodes)),
          m_sealed(other.m_sealed)
    {
        other.m_sealed = false;
    }

    ParallelGraph& operator=(ParallelGraph&& other) noexcept
    {
        if (this != &other) {
            clear();
            m_nodes = std::move(other.m_nodes);
            m_sealed = other.m_sealed;
            other.m_sealed = false;
        }
        return *this;
    }

    std::expected<ParallelNodeId, ParallelError> add(ParallelWork work) noexcept
    {
        if (m_sealed) {
            return std::unexpected(ParallelError(ParallelErrorCode::kInvalidGraph));
        }
        if (!work.valid()) {
            return std::unexpected(ParallelError(ParallelErrorCode::kAllocationFailed));
        }

        const auto id = m_nodes.size();
        auto* node = new (std::nothrow) Node(std::move(work));
        if (node == nullptr || !m_nodes.push_back(node)) {
            delete node;
            return std::unexpected(ParallelError(ParallelErrorCode::kAllocationFailed));
        }
        return id;
    }

    template <typename F>
    std::expected<ParallelNodeId, ParallelError> add(F&& function) noexcept
    {
        return add(makeParallelWork(std::forward<F>(function)));
    }

    std::expected<void, ParallelError> then(ParallelNodeId predecessor,
                                            ParallelNodeId successor) noexcept
    {
        if (m_sealed) {
            return std::unexpected(ParallelError(ParallelErrorCode::kInvalidGraph));
        }
        if (!validNode(predecessor) || !validNode(successor) || predecessor == successor) {
            return std::unexpected(ParallelError(ParallelErrorCode::kInvalidNode));
        }

        auto& successors = m_nodes[predecessor]->successors;
        if (std::find(successors.begin(), successors.end(), successor) != successors.end()) {
            return std::unexpected(ParallelError(ParallelErrorCode::kDuplicateDependency));
        }

        if (!successors.push_back(successor)) {
            return std::unexpected(ParallelError(ParallelErrorCode::kAllocationFailed));
        }
        ++m_nodes[successor]->predecessor_count;
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
        detail::NoThrowVector<ParallelNodeId> successors;
        std::size_t predecessor_count = 0;
        std::atomic<std::size_t> pending_predecessors{0};
        std::atomic<NodeState> state{NodeState::kPending};
        Node* validation_next = nullptr;
        ParallelError error{};
    };

    friend class detail::ParallelState;
    friend class detail::ParallelAwaitable;

    bool validNode(ParallelNodeId id) const noexcept
    {
        return id < m_nodes.size();
    }

    std::expected<void, ParallelError> validate() noexcept
    {
        if (m_nodes.empty()) {
            return {};
        }

        // Reuse per-node scratch state instead of allocating a temporary
        // indegree vector and ready queue. await_suspend() is noexcept, so
        // validation must not have an allocation failure path. The queue is
        // intrusive and uses a scratch link in each node, keeping validation
        // linear in nodes plus edges.
        Node* ready_head = nullptr;
        Node* ready_tail = nullptr;
        const auto enqueue_ready = [&ready_head, &ready_tail](Node* node) noexcept {
            node->validation_next = nullptr;
            if (ready_tail != nullptr) {
                ready_tail->validation_next = node;
            } else {
                ready_head = node;
            }
            ready_tail = node;
        };

        for (auto* node : m_nodes) {
            node->pending_predecessors.store(node->predecessor_count,
                                             std::memory_order_relaxed);
            node->state.store(NodeState::kPending, std::memory_order_relaxed);
            node->validation_next = nullptr;
            if (node->predecessor_count == 0) {
                enqueue_ready(node);
            }
        }

        std::size_t visited = 0;
        while (ready_head != nullptr) {
            auto* node = ready_head;
            ready_head = node->validation_next;
            if (ready_head == nullptr) {
                ready_tail = nullptr;
            }
            node->validation_next = nullptr;

            node->state.store(NodeState::kSucceeded, std::memory_order_relaxed);
            ++visited;
            for (const auto successor : node->successors) {
                auto* next = m_nodes[successor];
                if (next->pending_predecessors.fetch_sub(
                        1, std::memory_order_relaxed) == 1) {
                    enqueue_ready(next);
                }
            }
        }

        // The scratch values are not part of the graph's public state. Reset
        // them before either returning a cycle error or sealing the graph.
        for (auto* node : m_nodes) {
            node->state.store(NodeState::kPending, std::memory_order_relaxed);
            node->pending_predecessors.store(0, std::memory_order_relaxed);
            node->validation_next = nullptr;
        }

        if (visited != m_nodes.size()) {
            return std::unexpected(ParallelError(ParallelErrorCode::kCycleDetected));
        }
        return {};
    }

    void clear() noexcept
    {
        for (auto* node : m_nodes) {
            delete node;
        }
    }

    detail::NoThrowVector<Node*> m_nodes;
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
        for (auto* node : m_graph.m_nodes) {
            node->pending_predecessors.store(node->predecessor_count,
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
            if (m_graph.m_nodes[id]->predecessor_count == 0) {
                submitNode(id);
            }
        }
    }

    bool completed() const noexcept
    {
        return m_completed.load(std::memory_order_acquire);
    }

    std::expected<void, ParallelError> result() const noexcept
    {
        const auto error_node = m_first_error_node.load(std::memory_order_acquire);
        if (error_node == ParallelError::kNoNode) {
            return {};
        }

        const auto& error = m_graph.m_nodes[error_node]->error;
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
        auto& node = *m_graph.m_nodes[id];
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
        auto& node = *m_graph.m_nodes[id];
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
        auto& node = *m_graph.m_nodes[id];
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
        auto& node = *m_graph.m_nodes[id];
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
        auto& node = *m_graph.m_nodes[id];
        if (succeeded) {
            for (const auto successor : node.successors) {
                auto& next = *m_graph.m_nodes[successor];
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
            // The parent state is also retained by this ParallelState. Move
            // that reference out before waking the parent so a rejected wake
            // cannot leave a parent-frame -> ParallelState -> parent-state
            // reference cycle behind when the owner scheduler is stopped.
            TaskRef parent = std::move(m_parent);
            const auto resume_result =
                detail::requestTaskResumeStateDetailed(parent.state());
            if (resume_result == detail::TaskResumeResult::kRejected) {
                // The owner scheduler may have stopped before the last node
                // completed, making an owner-only resume impossible. Publish
                // the failure directly to the suspended parent so join/wait
                // observes a terminal result instead of waiting forever.
                auto* parent_state = parent.state();
                if (parent_state != nullptr &&
                    !parent_state->m_done.load(std::memory_order_acquire)) {
                    storeTaskError(parent_state,
                                   TaskResultError(
                                       TaskResultErrorCode::kResumeFailed));
                    completeTaskState(parent_state);
                }
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
            const auto error = m_graph.m_nodes[0]->work.invoke();
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
