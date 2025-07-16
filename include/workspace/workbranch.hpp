#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <unordered_map>
#include <memory>
#include <tuple>
#include <workspace/autothread.hpp>
#include <workspace/invoke.hpp>
#include <workspace/taskqueue.hpp>
#include <workspace/utility.hpp>

namespace wsp {

enum class waitstrategy {
    lowlatancy,  // Busy-wait with std::this_thread::yield(), minimal latency.
    balance,     // Busy-wait initially, then sleep briefly after max_spin_count.
    blocking     // Block thread using condition variables until a task is available
                 // or conditions are met.
};

namespace details {

class workbranch {
    friend class supervisor;
    constexpr static int max_spin_count = 10000;

    enum class WorkerState { None, Running, Waiting, Destructing };

    using worker_id = uintmax_t;
    using worker_map = std::unordered_map<worker_id, autothread<detach>>;

    std::atomic<worker_id> worker_next_id = 0;

    waitstrategy wait_strategy = {};

    std::atomic<size_t> decline = 0;
    size_t task_done_workers = 0;
    size_t waiting_finished_worker = 0;

    std::atomic<WorkerState> worker_state = WorkerState::None;

    worker_map workers = {};
    taskqueue<task_t> tq = {};

    std::mutex lok = {};
    std::condition_variable thread_cv = {};
    std::condition_variable task_done_cv = {};
    std::condition_variable task_cv = {};
    std::condition_variable waiting_finished = {};

public:
    /**
     * @brief construct function
     * @param wks initial number of workers
     * @param strategy wait_strategy for workers (defaults to blocking).
     */
    explicit workbranch(int wks = 1, waitstrategy strategy = waitstrategy::blocking) {
        wait_strategy = strategy;
        for (int i = 0; i < std::max(wks, 1); ++i) {
            add_worker();  // worker
        }
    }

    workbranch(const workbranch&) = delete;
    workbranch(workbranch&&) = delete;

    ~workbranch() {
        worker_state = WorkerState::Destructing;
        {
            std::lock_guard<std::mutex> lock(lok);
            decline = workers.size();
        }

        if (wait_strategy == waitstrategy::blocking) task_cv.notify_all();

        for (;;) {
            {
                std::lock_guard<std::mutex> lock(lok);
                if (decline > 0) {
                    for (auto it = workers.begin(); it != workers.end();) {
                        if (!it->second.is_alive()) {
                            it = workers.erase(it);
                            --decline;
                        } else {
                            ++it;
                        }
                    }
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

public:
    /**
     * @brief Wait for all tasks done.
     * @brief This interface will pause all threads(workers) in workbranch to
     * relieve system's stress.
     * @param timeout timeout for waiting
     * @return return true if all tasks done
     */
    bool wait_tasks(std::chrono::milliseconds timeout = std::chrono::milliseconds::max()) {
        bool res;
        {
            std::unique_lock<std::mutex> locker(lok);
            worker_state = WorkerState::Waiting;
            if (wait_strategy == waitstrategy::blocking) task_cv.notify_all();
            res = task_done_cv.wait_for(locker, timeout, [this] {
                return task_done_workers >= workers.size();  // use ">=" to avoid supervisor delete workers
            });
            task_done_workers = 0;
            worker_state = WorkerState::Running;
        }
        thread_cv.notify_all();  // recover

        std::unique_lock<std::mutex> locker(lok);
        waiting_finished.wait(locker, [this] { return waiting_finished_worker >= workers.size(); });
        waiting_finished_worker = 0;
        return res;
    }

public:
    /**
     * @brief get number of workers
     * @return number
     */
    size_t num_workers() {
        std::lock_guard<std::mutex> lock(lok);
        return workers.size();
    }

    /**
     * @brief get number of tasks in the task queue
     * @return number
     */
    size_t num_tasks() {
        return tq.length();
    }

public:
    /**
     * @brief Submit a task with arguments by binding them and forwarding to submit.
     * @param task Callable object.
     * @param args Arguments for the callable.
     */
    template <typename T = normal, typename F, typename... Args, typename R = details::result_of_t<F, Args...>>
    auto submit(F&& task, Args&&... args) -> auto {
        auto args_tuple = std::make_tuple(std::forward<Args>(args)...);
        auto task_lambda = [func = std::forward<F>(task), args_tuple = std::move(args_tuple)]() mutable -> R {
            return invoke_hpp::apply(func, args_tuple);
        };

        return submit<T>(std::move(task_lambda));
    }

    /**
     * @brief async execute the task
     * @tparam T Task priority tag: either `normal` (default) or `urgent`
     * @param task runnable object (normal)
     * @return void
     */
    template <typename T = normal, typename F, typename R = details::result_of_t<F>,
              typename DR = typename std::enable_if<std::is_void<R>::value>::type>
    auto submit(F&& task) -> typename std::enable_if<!std::is_same<T, sequence>::value>::type {
        auto wrapper_task = [task = std::forward<F>(task)]() mutable {
            try {
                task();
            } catch (const std::exception& ex) {
                std::cerr << "workspace: worker[" << std::this_thread::get_id()
                          << "] caught exception:\n  what(): " << ex.what() << '\n'
                          << std::flush;
            } catch (...) {
                std::cerr << "workspace: worker[" << std::this_thread::get_id() << "] caught unknown exception\n"
                          << std::flush;
            }
        };

        add_task<T>(std::move(wrapper_task));

        if (wait_strategy == waitstrategy::blocking) task_cv.notify_one();
    }

    /**
     * @brief async execute tasks
     * @param task runnable object (sequence)
     * @param tasks other parameters
     * @return void
     */
    template <typename T, typename F, typename... Fs>
    auto submit(F&& task, Fs&&... tasks) -> typename std::enable_if<std::is_same<T, sequence>::value>::type {
        tq.push_back([=] {
            try {
                this->rexec(task, tasks...);
            } catch (const std::exception& ex) {
                std::cerr << "workspace: worker[" << std::this_thread::get_id()
                          << "] caught exception:\n  what(): " << ex.what() << '\n'
                          << std::flush;
            } catch (...) {
                std::cerr << "workspace: worker[" << std::this_thread::get_id() << "] caught unknown exception\n"
                          << std::flush;
            }
        });
        if (wait_strategy == waitstrategy::blocking) task_cv.notify_one();
    }

    /**
     * @brief async execute the task
     * @tparam T Task priority tag: either `normal` (default) or `urgent`
     * @param task runnable object
     * @return std::future<R>
     */
    template <typename T = normal, typename F, typename R = details::result_of_t<F>,
              typename DR = typename std::enable_if<!std::is_void<R>::value, R>::type,
              typename = typename std::enable_if<!std::is_same<T, sequence>::value>::type>
    auto submit(F&& task) -> std::future<R> {
        auto task_ptr = std::make_shared<std::packaged_task<R()>>(std::forward<F>(task));
        auto future = task_ptr->get_future();
        auto wrapper_task = [task = std::move(task_ptr)] {
            try {
                (*task)();
            } catch (const std::exception& ex) {
                std::cerr << "workspace: worker[" << std::this_thread::get_id()
                          << "] caught exception:\n  what(): " << ex.what() << '\n'
                          << std::flush;
            } catch (...) {
                std::cerr << "workspace: worker[" << std::this_thread::get_id() << "] caught unknown exception\n"
                          << std::flush;
            }
        };

        add_task<T>(std::move(wrapper_task));

        if (wait_strategy == waitstrategy::blocking) task_cv.notify_one();
        return future;
    }

    /**
     * @brief submit a task asynchronously and always return a std::future,
     *        even if the task returns void.
     *
     * This function wraps the task and its arguments, runs it asynchronously,
     * and returns a future for its result.
     *
     * @tparam T Task priority tag: either `normal` (default) or `urgent`
     * @param task Callable object.
     * @param args Arguments for the callable.
     * @return std::future<R>
     *
     */
    template <typename T = normal, typename F, typename... Args, typename R = details::result_of_t<F, Args...>,
              typename = std::enable_if_t<!std::is_same<T, sequence>::value>>
    std::future<R> submit_future(F&& task, Args&&... args) {
        auto args_tuple = std::make_tuple(std::forward<Args>(args)...);
        auto task_lambda = [func = std::forward<F>(task), args_tuple = std::move(args_tuple)]() mutable -> R {
            return invoke_hpp::apply(func, args_tuple);
        };
        auto task_ptr = std::make_shared<std::packaged_task<R()>>(std::move(task_lambda));
        auto future = task_ptr->get_future();

        auto wrapper_task = [task_ptr = std::move(task_ptr)] {
            try {
                (*task_ptr)();
            } catch (const std::exception& ex) {
                std::cerr << "workspace: worker[" << std::this_thread::get_id()
                          << "] caught exception:\n  what(): " << ex.what() << '\n'
                          << std::flush;
            } catch (...) {
                std::cerr << "workspace: worker[" << std::this_thread::get_id() << "] caught unknown exception\n"
                          << std::flush;
            }
        };

        add_task<T>(std::move(wrapper_task));

        if (wait_strategy == waitstrategy::blocking) task_cv.notify_one();

        return future;
    }

private:
    template <typename T, typename Task>
    typename std::enable_if<std::is_same<T, normal>::value>::type add_task(Task&& task) {
        tq.push_back(std::forward<Task>(task));
    }

    template <typename T, typename Task>
    typename std::enable_if<!std::is_same<T, normal>::value>::type add_task(Task&& task) {
        tq.push_front(std::forward<Task>(task));
    }

    /**
     * @brief add one worker
     * @note O(logN)
     */
    void add_worker() {
        std::lock_guard<std::mutex> lock(lok);
        auto worker_id = worker_next_id.fetch_add(1);
        workers.try_emplace(worker_id, &workbranch::mission, this, worker_id);
    }

    /**
     * @brief delete one worker
     * @note O(1)
     */
    void del_worker(int num = 1) {
        std::lock_guard<std::mutex> lock(lok);
        if (workers.empty() && workers.size() < num) {
            throw std::runtime_error("workspace: No worker in workbranch to delete");
        } else {
            decline.fetch_add(num, std::memory_order_relaxed);
            task_cv.notify_all();
        }
    }

    // thread's default loop
    void mission(worker_id id) {
        task_t task;
        int spin_count = 0;

        while (true) {
            bool can_run = false;
            {
                std::lock_guard<std::mutex> lock(lok);

                if (decline <= 0) {
                    can_run = true;
                } else if (decline > 0) {
                    --decline;
                    workers.erase(id);

                    switch (worker_state) {
                        case WorkerState::Waiting: {
                            task_done_cv.notify_one();
                            break;
                        }
                        case WorkerState::Destructing: {
                            thread_cv.notify_one();
                            break;
                        }
                    }
                    return;
                }
            }

            if (can_run && tq.try_pop(task)) {
                task();
                spin_count = 0;
                continue;
            }

            if (worker_state == WorkerState::Waiting) {
                std::unique_lock<std::mutex> lock(lok);
                ++task_done_workers;
                task_done_cv.notify_one();

                thread_cv.wait(lock, [this] { return worker_state != WorkerState::Waiting; });

                ++waiting_finished_worker;
                if (waiting_finished_worker >= workers.size()) {
                    waiting_finished.notify_one();
                }
                continue;
            }

            switch (wait_strategy) {
                case waitstrategy::lowlatancy: {
                    std::this_thread::yield();
                    break;
                }
                case waitstrategy::balance: {
                    if (spin_count < max_spin_count) {
                        ++spin_count;
                        std::this_thread::yield();
                    } else {
                        // Just tell the system to suspend this thread in the shortest time
                        std::this_thread::sleep_for(std::chrono::nanoseconds(1));
                    }
                    break;
                }
                case waitstrategy::blocking: {
                    std::unique_lock<std::mutex> locker(lok);
                    task_cv.wait(locker, [this] {
                        return worker_state == WorkerState::Waiting || worker_state == WorkerState::Destructing ||
                               decline > 0 || num_tasks() > 0;
                    });
                    break;
                }
            }
        }
    }

    // recursive execute
    template <typename F>
    void rexec(F&& func) {
        func();
    }

    // recursive execute
    template <typename F, typename... Fs>
    void rexec(F&& func, Fs&&... funcs) {
        func();
        rexec(std::forward<Fs>(funcs)...);
    }
};

}  // namespace details
}  // namespace wsp