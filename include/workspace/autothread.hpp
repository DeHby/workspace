#pragma once
#include <atomic>
#include <thread>


#if defined(_WIN32)
#include <windows.h>
#elif defined(__unix__)
#include <errno.h>  // for errno
#include <pthread.h>
#include <signal.h>  // for ESRCH
#endif

namespace wsp {
namespace details {

struct join {};    // just for type inference
struct detach {};  // just for type inference

// thread wrapper
template <typename T>
class autothread {};

template <>
class autothread<join> {
    std::thread thrd;

public:
    template <typename F, typename... Args>
    explicit autothread(F&& f, Args&&... args)
      : thrd(std::forward<F>(f), std::forward<Args>(args)...) {
    }

    autothread(std::thread&& t)
      : thrd(std::move(t)) {
    }

    autothread(const autothread& other) = delete;
    autothread(autothread&& other) = default;
    ~autothread() {
        if (thrd.joinable()) thrd.join();
    }

    using id = std::thread::id;
    id get_id() {
        return thrd.get_id();
    }
};

template <>
class autothread<detach> {
    std::thread thrd;
    std::atomic_bool detach = false;

public:
    template <typename F, typename... Args>
    explicit autothread(F&& f, Args&&... args)
      : thrd(std::forward<F>(f), std::forward<Args>(args)...) {
    }

    autothread(std::thread&& t)
      : thrd(std::move(t)) {
    }
    autothread(const autothread& other) = delete;
    autothread(autothread&& other) = default;
    ~autothread() {
        if (thrd.joinable()) thrd.detach();
        detach = true;
    }

    using id = std::thread::id;
    id get_id() {
        return thrd.get_id();
    }

    bool is_alive() {
        if (detach) return false;
        if (thrd.joinable()) {
            auto handle = thrd.native_handle();
#if defined(_WIN32)
            DWORD exitCode = 0;
            if (GetExitCodeThread(handle, &exitCode)) {
                return exitCode == STILL_ACTIVE;
            }
#else
            return pthread_kill(handle, 0) == 0;
#endif
        }
        return false;
    }
};

}  // namespace details
}  // namespace wsp