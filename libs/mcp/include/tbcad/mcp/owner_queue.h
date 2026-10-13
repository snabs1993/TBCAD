#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

namespace tbcad::mcp {

// Hands work from server threads to the thread that owns the document (the UI thread). Server
// threads call run() and block; the owner calls pump() once per frame.
class OwnerQueue {
public:
    // The thread that constructs the queue is the owner.
    OwnerQueue() : owner_(std::this_thread::get_id()) {}
    ~OwnerQueue() { close(); }
    OwnerQueue(const OwnerQueue&) = delete;
    OwnerQueue& operator=(const OwnerQueue&) = delete;

    // Runs fn on the owner thread and waits. On the owner thread itself it runs inline. An
    // exception thrown by fn is rethrown here. Throws std::runtime_error once the queue is closed.
    void run(std::function<void()> fn);

    // Owner thread: runs queued work until the queue is empty or `budget` has passed (at least
    // one item runs). Returns how many items ran.
    std::size_t pump(std::chrono::milliseconds budget = std::chrono::milliseconds(50));

    bool hasPending() const;
    // Called after work is queued (e.g. glfwPostEmptyEvent), from the posting thread. Also called
    // by pump() when work is left over.
    void setWake(std::function<void()> wake);

    // Fails everything waiting and everything posted later.
    void close();

private:
    struct Item {
        std::function<void()> fn{};
        std::promise<void> done{};
    };

    std::thread::id owner_;
    mutable std::mutex mutex_{};
    std::deque<Item> items_{};
    std::function<void()> wake_{};
    bool closed_ = false;
};

}  // namespace tbcad::mcp
