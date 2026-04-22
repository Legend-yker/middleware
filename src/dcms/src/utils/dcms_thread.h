#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace dcms::utils {

// Continuously executes a task at a given period until requestStop().
// Destructor guarantees the worker thread is stopped and joined.
class LoopThread {
public:
    LoopThread() = default;
    LoopThread(std::string name, std::function<void()> task,
               std::chrono::milliseconds period);
    ~LoopThread();

    LoopThread(LoopThread &&o) noexcept;
    LoopThread &operator=(LoopThread &&o) noexcept;
    LoopThread(const LoopThread &)            = delete;
    LoopThread &operator=(const LoopThread &) = delete;

    int  requestStop();
    bool isRunning() const { return running_.load(); }
    const std::string &name() const { return name_; }

private:
    void run();

    std::string              name_;
    std::function<void()>    task_;
    std::chrono::milliseconds period_{0};
    std::atomic<bool>        stop_requested_{false};
    std::atomic<bool>        running_{false};
    std::thread              worker_;
};

// Executes a task exactly once in a background thread.
// Destructor joins the thread. Call wait() to block until done.
class SingleThread {
public:
    SingleThread() = default;
    SingleThread(std::string name, std::function<void()> task);
    ~SingleThread();

    SingleThread(SingleThread &&o) noexcept;
    SingleThread &operator=(SingleThread &&o) noexcept;
    SingleThread(const SingleThread &)            = delete;
    SingleThread &operator=(const SingleThread &) = delete;

    int  wait();
    bool isDone() const { return done_.load(); }
    const std::string &name() const { return name_; }

private:
    std::string       name_;
    std::atomic<bool> done_{true};
    std::thread       worker_;
};

} // namespace dcms::utils
