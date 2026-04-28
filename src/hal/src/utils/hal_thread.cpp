#include "utils/hal_thread.h"

namespace hal::utils {

// ---------- LoopThread ----------

LoopThread::LoopThread(std::string name, std::function<void()> task,
                       std::chrono::milliseconds period)
    : name_(std::move(name)), task_(std::move(task)), period_(period) {
    running_ = true;
    worker_ = std::thread([this] { run(); });
}

LoopThread::~LoopThread() {
    requestStop();
    if (worker_.joinable())
        worker_.join();
}

int LoopThread::requestStop() {
    stop_requested_ = true;
    return 0;
}

void LoopThread::run() {
    while (!stop_requested_.load()) {
        if (task_)
            task_();
        std::this_thread::sleep_for(period_);
    }
    running_ = false;
}

// ---------- SingleThread ----------

SingleThread::SingleThread(std::string name, std::function<void()> task)
    : name_(std::move(name)), done_(false) {
    worker_ = std::thread([this, t = std::move(task)] {
        if (t) t();
        done_ = true;
    });
}

SingleThread::~SingleThread() {
    if (worker_.joinable())
        worker_.join();
}

SingleThread::SingleThread(SingleThread &&o) noexcept
    : name_(std::move(o.name_)),
      done_(o.done_.load()),
      worker_(std::move(o.worker_)) {
    o.done_ = true;
}

SingleThread &SingleThread::operator=(SingleThread &&o) noexcept {
    if (this == &o) return *this;
    this->~SingleThread();
    new (this) SingleThread(std::move(o));
    return *this;
}

int SingleThread::wait() {
    if (worker_.joinable())
        worker_.join();
    return 0;
}

} // namespace hal::utils
