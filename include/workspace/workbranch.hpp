#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <memory>
#include <tuple>
#include <unordered_map>
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

		struct cpu_multiple_tag_t {};
		constexpr cpu_multiple_tag_t cpu_multiple_tag{};

		class workbranch {
			friend class supervisor;
			constexpr static int max_spin_count = 10000;

			struct worker_state {
				std::atomic_bool deleting = false;
				std::atomic_bool waiting = false;
				std::atomic_bool destructing = false;

				bool updated() {
					return deleting.load() || waiting.load() || destructing.load();
				}
			};

			class worker_info {
			public:
				worker_info() = default;

				autothread thread;
				bool busy = false;
				std::chrono::steady_clock::time_point last_active_time;

				template <typename F, typename... Args>
				worker_info(F&& f, Args&&... args)
					: busy(false)
					, thread(std::forward<F>(f), std::forward<Args>(args)...)
					, last_active_time(std::chrono::steady_clock::now()) {
				}

				void mark_idle() {
					busy = false;
					last_active_time = std::chrono::steady_clock::now();
				}

				void mark_busy() {
					busy = true;
				}

				bool is_idle() const {
					return !busy;
				}
			};

			using worker_id = uintmax_t;
			using worker_map = std::unordered_map<worker_id, worker_info>;

			std::atomic<worker_id> worker_next_id = 0;

			waitstrategy wait_strategy;

			std::atomic_size_t idle_workers = 0;
			std::atomic_size_t resumed_workers = 0;
			std::atomic_size_t pending_deletions = 0;

			worker_state worker_state;

			worker_map workers;
			taskqueue<task_t> tq;

			std::mutex lok;
			std::condition_variable thread_cv;
			std::condition_variable task_cv;

			std::condition_variable task_idle_cv;
			std::condition_variable task_resume_cv;

			std::condition_variable task_deletion_cv;

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
				{
					std::lock_guard<std::mutex> lock(lok);
					pending_deletions = workers.size();
					worker_state.destructing.store(true);

					if (wait_strategy == waitstrategy::blocking) {
						task_cv.notify_all();
					}
				}

				for (;;) {
					if (pending_deletions > 0) {
						std::lock_guard<std::mutex> lock(lok);

						for (auto it = workers.begin(); it != workers.end();) {
							if (!it->second.thread.is_alive()) {
								it = workers.erase(it);
								--pending_deletions;
							}
							else {
								++it;
							}
						}
					}
					else {
						break;
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
			bool wait_tasks(std::chrono::milliseconds timeout = default_max_time) {
				if (worker_state.destructing.load()) {
					return false;
				}

				bool res;
				{
					std::unique_lock<std::mutex> locker(lok);

					idle_workers = 0;
					worker_state.waiting.store(true);

					if (wait_strategy == waitstrategy::blocking) {
						task_cv.notify_all();
					}

					res = task_idle_cv.wait_for(locker, timeout, [this] {
						return idle_workers >= workers.size();  // use ">=" to avoid supervisor delete workers
					});

					worker_state.waiting.store(false);
				}

				thread_cv.notify_all();  // recover

				std::unique_lock<std::mutex> locker(lok);
				task_resume_cv.wait(locker, [this] { return resumed_workers >= idle_workers; });
				resumed_workers = 0;
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

			template <typename Rep, typename Period>
			size_t count_idle_workers(std::chrono::duration<Rep, Period> timeout) {
				std::lock_guard<std::mutex> lock(lok);
				size_t count = 0;
				auto now = std::chrono::steady_clock::now();
				for (const auto& worker_info : workers) {
					auto& worker = worker_info.second;
					if (worker.is_idle() &&
						std::chrono::duration_cast<std::chrono::milliseconds>(now - worker.last_active_time) >= timeout) {
						++count;
					}
				}
				return count;
			}

			size_t count_busy_workers() {
				std::lock_guard<std::mutex> lock(lok);
				size_t count = 0;
				for (const auto& worker_info : workers) {
					auto& worker = worker_info.second;
					if (!worker.is_idle()) {
						++count;
					}
				}
				return count;
			}

		public:
			/**
			 * @brief submit a task with arguments by binding them and forwarding to submit.
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
					}
					catch (const std::exception& ex) {
						std::cerr << "workspace: worker[" << std::this_thread::get_id()
							<< "] caught exception:\n  what(): " << ex.what() << '\n'
							<< std::flush;
					}
					catch (...) {
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
					}
					catch (const std::exception& ex) {
						std::cerr << "workspace: worker[" << std::this_thread::get_id()
							<< "] caught exception:\n  what(): " << ex.what() << '\n'
							<< std::flush;
					}
					catch (...) {
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
					}
					catch (const std::exception& ex) {
						std::cerr << "workspace: worker[" << std::this_thread::get_id()
							<< "] caught exception:\n  what(): " << ex.what() << '\n'
							<< std::flush;
					}
					catch (...) {
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
					}
					catch (const std::exception& ex) {
						std::cerr << "workspace: worker[" << std::this_thread::get_id()
							<< "] caught exception:\n  what(): " << ex.what() << '\n'
							<< std::flush;
					}
					catch (...) {
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
			void add_worker(size_t num = 1) {
				std::lock_guard<std::mutex> lock(lok);
				for (size_t i = 0; i < num; i++) {
					auto worker_id = worker_next_id.fetch_add(1);
					workers.try_emplace(worker_id, &workbranch::mission, this, worker_id);
				}
			}

			/**
			 * @brief delete one worker
			 * @note O(1)
			 */
			void del_worker(size_t num = 1) {
				{
					std::lock_guard<std::mutex> lock(lok);
					if (workers.empty() || workers.size() < num) {
						return;
					}
				}
				std::unique_lock<std::mutex> lock(lok);

				pending_deletions += num;
				worker_state.deleting.store(true);

				if (wait_strategy == waitstrategy::blocking) {
					task_cv.notify_all();
				}

				task_deletion_cv.wait(lock, [this] { return pending_deletions == 0; });

				worker_state.deleting.store(false);
			}

			void wait_for_task(int& spin_count) {
				switch (wait_strategy) {
					case waitstrategy::lowlatancy: {
							std::this_thread::yield();
							break;
						}
					case waitstrategy::balance: {
							if (spin_count < max_spin_count) {
								++spin_count;
								std::this_thread::yield();
							}
							else {
								// Just tell the system to suspend this thread in the shortest time
								std::this_thread::sleep_for(std::chrono::milliseconds(1));
							}
							break;
						}
					case waitstrategy::blocking: {
							std::unique_lock<std::mutex> locker(lok);
							task_cv.wait(locker, [this] { return num_tasks() > 0 || worker_state.updated(); });
							break;
						}
				}
			}

			bool check_declining(worker_id id) {
				std::lock_guard<std::mutex> lock(lok);
				if (pending_deletions.load() > 0) {
					--pending_deletions;
					workers.erase(id);
					if (worker_state.waiting.load()) {
						task_idle_cv.notify_one();
					}
					if (worker_state.destructing.load()) {
						thread_cv.notify_one();
					}
					task_deletion_cv.notify_one();
					return true;
				}

				return false;
			}

			void wait_resume(worker_id id) {
				std::unique_lock<std::mutex> lock(lok);
				idle_workers.fetch_add(1, std::memory_order_relaxed);
				task_idle_cv.notify_one();

				thread_cv.wait(lock, [this] { return !worker_state.waiting.load(); });

				resumed_workers.fetch_add(1, std::memory_order_relaxed);
				task_resume_cv.notify_one();
			}

			// thread's default loop
			void mission(worker_id id) {
				task_t task;
				int spin_count = 0;

				while (true) {
					if (worker_state.destructing.load() || worker_state.deleting.load()) {
						if (check_declining(id)) {
							return;
						}
					}

					if (tq.try_pop(task)) {
						{
							std::lock_guard<std::mutex> lock(lok);
							workers[id].mark_busy();
						}

						task();
						spin_count = 0;

						{
							std::lock_guard<std::mutex> lock(lok);
							workers[id].mark_idle();
						}
					}
					else if (worker_state.waiting.load()) {
						wait_resume(id);
					}

					wait_for_task(spin_count);
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