#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace hal::utils {

// Continuously executes a task at a given period.
// Manages its own lifecycle: destructor guarantees the worker thread is
// stopped and joined, so the caller only needs to construct / destruct.
class LoopThread {
public:
    LoopThread(std::string name, std::function<void()> task,
               std::chrono::milliseconds period);
    ~LoopThread();

    LoopThread(const LoopThread &) = delete;
    LoopThread &operator=(const LoopThread &) = delete;

    int  requestStop();
    bool isRunning() const { return running_.load(); }
    const std::string &name() const { return name_; }

private:
    void run();

    std::string name_;
    std::function<void()> task_;
    std::chrono::milliseconds period_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
};

// Executes a task exactly once in a detached background thread.
// The task is fire-and-forget; optionally wait for completion via wait().
class SingleThread {
public:
    SingleThread(std::string name, std::function<void()> task);
    ~SingleThread();

    SingleThread(const SingleThread &) = delete;
    SingleThread &operator=(const SingleThread &) = delete;

    int  wait();
    bool isDone() const { return done_.load(); }
    const std::string &name() const { return name_; }

private:
    std::string name_;
    std::atomic<bool> done_{false};
    std::thread worker_;
};

} // namespace hal::utils
