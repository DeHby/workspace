#pragma once
#include <memory>

#include "supervisor.hpp"
#include "workbranch.hpp"

namespace wsp {
namespace details {
/**
 * @brief A dynamic thread pool branch with automatic scaling support.
 *
 * `dynbranch` wraps a `workbranch` and supervises it using a `supervisor`,
 * automatically adjusting the number of workers based on workload.
 *
 * supports setting fixed worker ranges or using CPU core multiples for adaptive scaling.
 * allows submitting tasks, suspending/resuming supervision, and changing limits on the fly.
 */
class dynbranch {
    constexpr static auto default_time_idle = std::chrono::milliseconds(5000);
    constexpr static auto default_time_interval = std::chrono::milliseconds(1000);

    std::shared_ptr<workbranch> branch;
    std::unique_ptr<supervisor> supervisor;

public:
    /**
     * @brief construct a dynbranch with fixed worker limits.
     *
     * @param min_workers      minimum number of worker threads.
     * @param max_workers      maximum number of worker threads.
     * @param strategy         wait strategy (e.g., blocking or spinning).
     * @param idle_timeout timeout (ms) for idle worker detection
     * @param time_interval interval (ms) between supervision checks
     */
    explicit dynbranch(int min_workers = 1, int max_workers = std::max(2u, std::thread::hardware_concurrency()),
                       waitstrategy strategy = waitstrategy::blocking,
                       std::chrono::milliseconds idle_timeout = default_time_idle,
                       std::chrono::milliseconds time_interval = default_time_interval)
      : branch(std::make_shared<details::workbranch>(1, strategy))
      , supervisor(std::make_unique<details::supervisor>(idle_timeout, time_interval)) {
        supervisor->supervise(branch, min_workers, max_workers);
    }

    /**
     * @brief construct a dynbranch using CPU-core-multiplied worker limits.
     *
     * @param min_core_mult    Minimum thread count = core count × min_core_mult.
     * @param max_core_mult    Maximum thread count = core count × max_core_mult.
     * @param strategy         Wait strategy.
     * @param idle_timeout timeout (ms) for idle worker detection
     * @param time_interval interval (ms) between supervision checks
     */
    dynbranch(cpu_multiple_tag_t, double min_core_mult = 1, double max_core_mult = 2,
              waitstrategy strategy = waitstrategy::blocking,
              std::chrono::milliseconds idle_timeout = default_time_idle,
              std::chrono::milliseconds time_interval = default_time_interval)
      : branch(std::make_shared<details::workbranch>(1, strategy))
      , supervisor(std::make_unique<details::supervisor>(idle_timeout, time_interval)) {
        supervisor->supervise(branch, cpu_multiple_tag, min_core_mult, max_core_mult, time_interval);
    }

    dynbranch(const dynbranch&) = delete;
    dynbranch(dynbranch&&) = delete;

    template <typename T = normal, typename F, typename... Args, typename R = details::result_of_t<F, Args...>>
    auto submit(F&& task, Args&&... args) -> auto {
        return branch->submit<T>(std::forward<F>(task), std::forward<Args>(args)...);
    }

    template <typename T = normal, typename F, typename R = details::result_of_t<F>,
              typename DR = typename std::enable_if<std::is_void<R>::value>::type>
    auto submit(F&& task) -> typename std::enable_if<!std::is_same<T, sequence>::value>::type {
        return branch->submit<T>(std::forward<F>(task));
    }

    template <typename T, typename F, typename... Fs>
    auto submit(F&& task, Fs&&... tasks) -> typename std::enable_if<std::is_same<T, sequence>::value>::type {
        return branch->submit<T>(std::forward<F>(task), std::forward<Fs>(tasks)...);
    }

    template <typename T = normal, typename F, typename R = details::result_of_t<F>,
              typename DR = typename std::enable_if<!std::is_void<R>::value, R>::type,
              typename = typename std::enable_if<!std::is_same<T, sequence>::value>::type>
    auto submit(F&& task) -> std::future<R> {
        return branch->submit<T>(std::forward<F>(task));
    }

    template <typename T = normal, typename F, typename... Args, typename R = details::result_of_t<F, Args...>,
              typename = std::enable_if_t<!std::is_same<T, sequence>::value>>
    std::future<R> submit_future(F&& task, Args&&... args) {
        return branch->submit_future<T>(std::forward<F>(task), std::forward<Args>(args)...);
    }

    /**
     * @brief wait for all tasks to complete or timeout.
     *
     * @param timeout The timeout duration (default is maximum wait).
     * @return true if all tasks finished before timeout.
     */
    bool wait_tasks(std::chrono::milliseconds timeout = default_max_time) {
        return branch->wait_tasks(timeout);
    }

    /**
     * @brief get current number of workers.
     */
    size_t num_workers() {
        return branch->num_workers();
    }

    /**
     * @brief get current number of pending tasks.
     */
    size_t num_tasks() {
        return branch->num_tasks();
    }

    /**
     * @brief pause supervision and extend the wait interval.
     *
     * @param timeout Maximum time to wait while paused.
     */
    void suspend(std::chrono::milliseconds timeout = default_max_time) {
        supervisor->suspend(timeout);
    }

    /**
     * @brief resume supervision immediately.
     */
    void proceed() {
        supervisor->proceed();
    }

    /**
     * @brief set a callback to be called on every supervision tick.
     *
     * @param cb Callback function.
     */
    void set_tick_cb(std::function<void()> cb) {
        supervisor->set_tick_cb(std::move(cb));
    }

    /**
     * @brief update min/max worker limits at runtime.
     * @param idle_timeout timeout (ms) for idle worker detection
     */
    void set_worker_limits(int min_workers, int max_workers,
                           std::chrono::milliseconds idle_timeout = default_time_idle) {
        supervisor->supervise(branch, min_workers, max_workers, idle_timeout);
    }

    /**
     * @brief update worker limits using core-multiplier strategy.
     */
    void set_worker_limits(cpu_multiple_tag_t, double min_core_mult, double max_core_mult) {
        supervisor->supervise(branch, cpu_multiple_tag, min_core_mult, max_core_mult);
    }
};
}  // namespace details
}  // namespace wsp