// The worker pool that keeps the UI from ever blocking.
//
// The window loop is single-threaded and event-driven; anything slow -- compiling
// a sketch, flashing a board, stepping the emulator -- would freeze it if run
// there. Jobs runs that work on a pool of background threads instead. When a job
// finishes it queues a completion callback and wakes the UI loop with a custom
// SDL event, so the callback (and only it) runs back on the UI thread. Worker
// code never touches the renderer or UI state; the completion callback is where
// results cross back, on the one thread that owns the screen.
//
// This is the single rule that makes the whole app safe: heavy work off-thread,
// all UI mutation on-thread, the two joined by a wake event.
#pragma once
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace studio {

class Jobs {
public:
    // Spawns the pool. `threads` == 0 picks a sensible count from the hardware.
    explicit Jobs(unsigned threads = 0);
    ~Jobs();

    Jobs(const Jobs&) = delete;
    Jobs& operator=(const Jobs&) = delete;

    // Runs `work` on a worker thread. When it returns, `on_done` (if given) is
    // queued for the UI thread and the loop is woken. `work` MUST NOT touch the
    // renderer, fonts, or any UI state -- pass results back through `on_done`.
    void run(std::function<void()> work, std::function<void()> on_done = {});

    // Called on the UI thread after a wake event: runs every completion callback
    // that has arrived. Returns how many ran.
    int drain();

    // The SDL event type pushed to wake the UI thread; compare against it in the
    // event loop and call drain() when it arrives.
    uint32_t wake_event() const { return wake_event_; }

    unsigned worker_count() const { return static_cast<unsigned>(workers_.size()); }

private:
    struct Task {
        std::function<void()> work;
        std::function<void()> on_done;
    };
    void worker_loop();

    std::vector<std::thread> workers_;
    std::queue<Task> work_;
    std::queue<std::function<void()>> done_;
    std::mutex work_mtx_;
    std::mutex done_mtx_;
    std::condition_variable work_cv_;
    bool stop_ = false;
    uint32_t wake_event_ = 0;
};

}  // namespace studio
