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
class autothread {
    std::thread thrd;
    std::atomic_bool detach = false;

public:
    autothread() noexcept = default;

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
            DWORD waitResult = WaitForSingleObject(handle, 0);
            return waitResult == WAIT_TIMEOUT;
#else
            return pthread_kill(handle, 0) == 0;
#endif
        }
        return false;
    }
};

}  // namespace details
}  // namespace wsp