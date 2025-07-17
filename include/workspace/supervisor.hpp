#pragma once
#include <atomic>
#include <cassert>
#include <cmath>
#include <condition_variable>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>
#include <workspace/utility.hpp>
#include <workspace/workbranch.hpp>

namespace wsp {
namespace details {
// workbranch supervisor
class supervisor {
    using tick_callback_t = std::function<void()>;
    constexpr static auto default_time_idle = std::chrono::milliseconds(5000);
    constexpr static auto default_time_interval = std::chrono::milliseconds(1000);

    struct BranchLimits {
        std::shared_ptr<workbranch> branch;
        size_t min;
        size_t max;
        std::chrono::milliseconds idle_timeout;
    };

    std::atomic_bool stop = false;

    size_t wmin = 0;
    size_t wmax = 0;
    std::chrono::milliseconds tout;
    std::chrono::milliseconds tval;

    std::chrono::milliseconds default_idle_timeout;

    tick_callback_t tick_cb;

    std::mutex spv_lok;
    std::condition_variable thrd_cv;

    std::thread worker;
    std::vector<BranchLimits> branch_limits;

public:
    /**
     * @brief construct a supervisor with specified worker limits and intervals
     * @param min_wokrs minimum number of workers
     * @param max_wokrs maximum number of workers
     * @param idle_timeout timeout (ms) for idle worker detection
     * @param time_interval interval (ms) between supervision checks
     */
    explicit supervisor(int min_wokrs, int max_wokrs, std::chrono::milliseconds idle_timeout = default_time_idle,
                        std::chrono::milliseconds time_interval = default_time_interval)
      : wmin(min_wokrs)
      , wmax(max_wokrs)
      , tout(time_interval)
      , tval(time_interval)
      , tick_cb(nullptr)
      , default_idle_timeout(idle_timeout) {
        assert(min_wokrs >= 0 && max_wokrs > 0 && max_wokrs > min_wokrs);
        worker = std::thread(&supervisor::mission, this);
    }

    /**
     * @brief construct a supervisor with default worker limits
     * @param idle_timeout timeout (ms) for idle worker detection
     * @param time_interval interval (ms) between supervision checks
     * @note uses 1 as min workers and max(2, hardware_concurrency) as max workers
     */
    explicit supervisor(std::chrono::milliseconds idle_timeout = default_time_idle,
                        std::chrono::milliseconds time_interval = default_time_interval)
      : supervisor(1, std::max(2u, std::thread::hardware_concurrency()), idle_timeout, time_interval) {
    }

    /**
     * @brief construct a supervisor with worker limits based on cpu core multiples
     * @param tag tag for cpu core scaling mode
     * @param min_core_mult multiple for minimum workers
     * @param max_core_mult multiple for maximum workers
     * @param idle_timeout timeout (ms) for idle worker detection
     * @param time_interval interval (ms) between supervision checks
     */
    explicit supervisor(cpu_multiple_tag_t, double min_core_mult, double max_core_mult,
                        std::chrono::milliseconds idle_timeout = default_time_idle,
                        std::chrono::milliseconds time_interval = default_time_interval)
      : supervisor(static_cast<int>(std::ceil(std::max(1u, std::thread::hardware_concurrency()) * min_core_mult)),
                   static_cast<int>(std::ceil(std::max(1u, std::thread::hardware_concurrency()) * max_core_mult)),
                   idle_timeout, time_interval) {
    }

    supervisor(const supervisor&) = delete;
    supervisor(supervisor&&) = delete;

    /**
     * @brief destroy the supervisor and stop supervision
     * @note joins the supervisor thread after setting stop flag
     */
    ~supervisor() {
        {
            std::lock_guard<std::mutex> lock(spv_lok);
            stop = true;
        }
        if (worker.joinable()) {
            worker.join();  // wait for the loop to finish
        }
    }

public:
    /**
     * @brief start supervising a workbranch with specified limits
     * @param wbr shared pointer to the workbranch
     * @param min_workers minimum number of workers
     * @param max_workers maximum number of workers
     * @param idle_timeout timeout (ms) for idle worker detection
     */
    void supervise(std::shared_ptr<workbranch> wbr, size_t min_workers, size_t max_workers,
                   std::chrono::milliseconds idle_timeout = default_time_idle) {
        std::lock_guard<std::mutex> lock(spv_lok);
        auto it = std::find_if(branch_limits.begin(), branch_limits.end(),
                               [&wbr](const BranchLimits& bl) { return bl.branch == wbr; });
        if (it == branch_limits.end()) {
            BranchLimits bl{wbr, min_workers, max_workers, idle_timeout};
            branch_limits.emplace_back(std::move(bl));
        } else {
            it->min = min_workers;
            it->max = max_workers;
            it->idle_timeout = idle_timeout;
        }
    }

    /**
     * @brief start supervising a workbranch with default limits
     * @param wbr shared pointer to the workbranch
     * @note uses supervisor's default min, max, and idle_timeout
     */
    void supervise(std::shared_ptr<workbranch> wbr) {
        supervise(std::move(wbr), wmin, wmax, default_idle_timeout);
    }

    /**
     * @brief start supervising a workbranch with cpu core-based limits
     * @param wbr shared pointer to the workbranch
     * @param tag tag for cpu core scaling mode
     * @param min_core_mult multiple for minimum workers
     * @param max_core_mult multiple for maximum workers
     * @param idle_timeout timeout (ms) for idle worker detection
     */
    void supervise(std::shared_ptr<workbranch> wbr, cpu_multiple_tag_t, double min_core_mult, double max_core_mult,
                   std::chrono::milliseconds idle_timeout = default_time_idle) {
        auto cores = (std::max)(1u, std::thread::hardware_concurrency());
        auto min = static_cast<size_t>(std::ceil(cores * min_core_mult));
        auto max = static_cast<size_t>(std::ceil(cores * max_core_mult));
        supervise(std::move(wbr), min, max, idle_timeout);
    }

    /**
     * @brief suspend the supervisor for a specified duration
     * @param timeout duration (ms) to suspend supervision
     */
    void suspend(std::chrono::milliseconds timeout = default_max_time) {
        std::lock_guard<std::mutex> lock(spv_lok);
        tout = timeout;
    }

    /**
     * @brief resume supervision with original interval
     */
    void proceed() {
        std::lock_guard<std::mutex> lock(spv_lok);
        tout = tval;
    }

    /**
     * @brief set callback to execute before each supervision rest
     * @param cb callback function
     */
    void set_tick_cb(tick_callback_t cb) {
        tick_cb = cb;
    }

private:
    // loop func
    void mission() {
        std::chrono::steady_clock::time_point last_tick = std::chrono::steady_clock::now();
        while (!stop) {
            try {
                {
                    std::lock_guard<std::mutex> lock(spv_lok);
                    for (auto& limit : branch_limits) {
                        auto& branch = limit.branch;
                        // get info
                        auto tknums = branch->num_tasks();
                        auto wknums = branch->num_workers();
                        // adjust

                        if (wknums > limit.max) {
                            auto num = wknums - limit.max;
                            branch->del_worker(num);
                            continue;
                        }

                        if (tknums) {
                            size_t nums = std::min(limit.max - wknums, tknums - wknums);
                            branch->add_worker(nums);  // quick add

                        } else if (wknums > limit.min) {
                            auto size = branch->count_idle_workers(limit.idle_timeout);
                            if (size > limit.min) {
                                branch->del_worker(size - limit.min);  // quick dec
                            }
                        }
                    }
                }

                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick) >= tout) {
                    last_tick = now;
                    if (tick_cb) tick_cb();  // execute tick callback
                }

            } catch (const std::exception& e) {
                std::cerr << "workspace: supervisor[" << std::this_thread::get_id() << "] caught exception:\n  \
                what(): " << e.what()
                          << '\n'
                          << std::flush;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

}  // namespace details
}  // namespace wsp