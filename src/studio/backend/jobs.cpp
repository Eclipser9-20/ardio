#include "studio/backend/jobs.h"

#include <SDL3/SDL.h>

namespace studio {

Jobs::Jobs(unsigned threads) {
    wake_event_ = SDL_RegisterEvents(1);
    if (threads == 0) {
        unsigned hw = std::thread::hardware_concurrency();
        threads = hw > 2 ? hw - 1 : 2;  // leave a core for the UI thread
    }
    for (unsigned i = 0; i < threads; ++i)
        workers_.emplace_back([this] { worker_loop(); });
}

Jobs::~Jobs() {
    {
        std::lock_guard<std::mutex> lk(work_mtx_);
        stop_ = true;
    }
    work_cv_.notify_all();
    for (auto& t : workers_)
        if (t.joinable()) t.join();
}

void Jobs::run(std::function<void()> work, std::function<void()> on_done) {
    {
        std::lock_guard<std::mutex> lk(work_mtx_);
        work_.push(Task{std::move(work), std::move(on_done)});
    }
    work_cv_.notify_one();
}

void Jobs::worker_loop() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(work_mtx_);
            work_cv_.wait(lk, [this] { return stop_ || !work_.empty(); });
            if (stop_ && work_.empty()) return;
            task = std::move(work_.front());
            work_.pop();
        }
        if (task.work) task.work();
        if (task.on_done) {
            {
                std::lock_guard<std::mutex> lk(done_mtx_);
                done_.push(std::move(task.on_done));
            }
            // Wake the UI loop so it drains the completion on its own thread.
            SDL_Event e;
            SDL_zero(e);
            e.type = wake_event_;
            SDL_PushEvent(&e);
        }
    }
}

int Jobs::drain() {
    std::queue<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> lk(done_mtx_);
        std::swap(ready, done_);
    }
    int n = 0;
    while (!ready.empty()) {
        if (ready.front()) ready.front()();
        ready.pop();
        ++n;
    }
    return n;
}

}  // namespace studio
