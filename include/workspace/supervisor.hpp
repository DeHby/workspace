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
struct cpu_multiple_tag_t {};
constexpr static cpu_multiple_tag_t cpu_multiple_tag{};
// workbranch supervisor
class supervisor {
    using tick_callback_t = std::function<void()>;

private:
    struct BranchLimits {
        workbranch* branch;
        size_t min;
        size_t max;
    };

    std::atomic<bool> stop = false;

    size_t wmin = 0;
    size_t wmax = 0;
    unsigned tout = 0;
    const unsigned tval = 0;

    tick_callback_t tick_cb;

    std::mutex spv_lok;
    std::condition_variable thrd_cv;

    std::unique_ptr<autothread<join>> worker;
    std::vector<BranchLimits> branches;

public:
    /**
     * @brief construct a supervisor
     * @param min_wokrs min nums of workers
     * @param max_wokrs max nums of workers
     * @param time_interval  time interval between each check
     */
    explicit supervisor(int min_wokrs, int max_wokrs, unsigned time_interval = 500)
      : wmin(min_wokrs)
      , wmax(max_wokrs)
      , tout(time_interval)
      , tval(time_interval)
      , tick_cb(nullptr) {
        assert(min_wokrs >= 0 && max_wokrs > 0 && max_wokrs > min_wokrs);
        worker = std::make_unique<autothread<join>>(&supervisor::mission, this);
    }

    /**
     * @brief construct a supervisor with default min/max based on hardware concurrency
     * @param time_interval interval (ms) between each supervision check
     */
    explicit supervisor(unsigned time_interval = 500)
      : supervisor(1, std::max(2u, std::thread::hardware_concurrency()), time_interval) {
    }

    /**
     * @brief construct supervisor using cpu core multiples
     * @param tag tag type for dispatch
     * @param minCoreMultiple multiple of min workers
     * @param maxCoreMultiple multiple of max workers
     * @param time_interval interval (ms) between each supervision check
     */
    explicit supervisor(cpu_multiple_tag_t, double minCoreMultiple, double maxCoreMultiple,
                        unsigned time_interval = 500)
      : supervisor(static_cast<int>(std::ceil(std::max(1u, std::thread::hardware_concurrency()) * minCoreMultiple)),
                   static_cast<int>(std::ceil(std::max(1u, std::thread::hardware_concurrency()) * maxCoreMultiple)),
                   time_interval) {
    }

    supervisor(const supervisor&) = delete;
    supervisor(supervisor&&) = delete;
    ~supervisor() {
        {
            std::lock_guard<std::mutex> lock(spv_lok);
            stop = true;
            thrd_cv.notify_one();
        }
    }

public:
    /**
     * @brief start supervising a workbranch with specified min and max workers
     * @param wbr Reference to the workbranch to supervise
     * @param min_wokrs min nums of workers
     * @param max_wokrs max nums of workers
     */
    void supervise(workbranch& wbr, int min_wokrs, int max_wokrs) {
        std::lock_guard<std::mutex> lock(spv_lok);

        BranchLimits branch;
        branch.branch = &wbr;
        branch.min = min_wokrs;
        branch.max = max_wokrs;
        branches.emplace_back(std::move(branch));
    }

    /**
     * @brief start supervising a workbranch with default min/max workers
     * @param wbr Reference to the workbranch to supervise
     *
     * Uses the supervisor's default min/max worker limits.
     */
    void supervise(workbranch& wbr) {
        supervise(wbr, static_cast<int>(wmin), static_cast<int>(wmax));
    }

    /**
     * @brief start supervising a workbranch with worker limits based on CPU cores
     * @param wbr Reference to the workbranch to supervise
     * @param tag Tag to indicate CPU core scaling mode
     * @param minCoreMultiple min workers = ceil(cores * minCoreMultiple)
     * @param maxCoreMultiple max workers = ceil(cores * maxCoreMultiple)
     */
    void supervise(workbranch& wbr, cpu_multiple_tag_t, double minCoreMultiple, double maxCoreMultiple) {
        auto cores = (std::max)(1u, std::thread::hardware_concurrency());
        int min = static_cast<int>(std::ceil(cores * minCoreMultiple));
        int max = static_cast<int>(std::ceil(cores * maxCoreMultiple));
        supervise(wbr, min, max);
    }

    /**
     * @brief suspend the supervisor
     * @param timeout the longest waiting time
     */
    void suspend(unsigned timeout = std::numeric_limits<unsigned>::max()) {
        std::lock_guard<std::mutex> lock(spv_lok);
        tout = timeout;
    }

    // go on supervising
    void proceed() {
        {
            std::lock_guard<std::mutex> lock(spv_lok);
            tout = tval;
        }
        thrd_cv.notify_one();
    }

    /**
     * @brief Always execute callback before taking a rest
     * @param cb callback function
     */
    void set_tick_cb(tick_callback_t cb) {
        tick_cb = cb;
    }

private:
    // loop func
    void mission() {
        while (!stop) {
            try {
                {
                    std::unique_lock<std::mutex> lock(spv_lok);
                    for (auto& pbr : branches) {
                        auto& branch = pbr.branch;
                        // get info
                        auto tknums = branch->num_tasks();
                        auto wknums = branch->num_workers();
                        // adjust
                        if (tknums) {
                            assert(wknums <= pbr.max);  // Avoid wrong usage
                            size_t nums = std::min(pbr.max - wknums, tknums - wknums);
                            for (size_t i = 0; i < nums; ++i) {
                                branch->add_worker();  // quick add
                            }
                        } else if (wknums > pbr.min) {
                            branch->del_worker();  // slow dec
                        }
                    }
                    if (!stop) thrd_cv.wait_for(lock, std::chrono::milliseconds(tout));
                }
                if (tick_cb) tick_cb();  // execute tick callback

            } catch (const std::exception& e) {
                std::cerr << "workspace: supervisor[" << std::this_thread::get_id() << "] caught exception:\n  \
                what(): " << e.what()
                          << '\n'
                          << std::flush;
            }
        }
    }
};

}  // namespace details
}  // namespace wsp