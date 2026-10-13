#include "tbcad/mcp/owner_queue.h"

#include <stdexcept>

namespace tbcad::mcp {

void OwnerQueue::run(std::function<void()> fn) {
    if (std::this_thread::get_id() == owner_) {
        fn();
        return;
    }
    std::future<void> done;
    std::function<void()> wake;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_)
            throw std::runtime_error("TBCAD is shutting down");
        Item item;
        item.fn = std::move(fn);
        done = item.done.get_future();
        items_.push_back(std::move(item));
        wake = wake_;
    }
    if (wake)
        wake();
    done.get();
}

std::size_t OwnerQueue::pump(std::chrono::milliseconds budget) {
    auto start = std::chrono::steady_clock::now();
    std::size_t ran = 0;
    for (;;) {
        Item item;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (items_.empty())
                return ran;
            item = std::move(items_.front());
            items_.pop_front();
        }
        try {
            item.fn();
            item.done.set_value();
        } catch (...) {
            item.done.set_exception(std::current_exception());
        }
        ++ran;
        if (std::chrono::steady_clock::now() - start >= budget) {
            std::function<void()> wake;
            bool more;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                more = !items_.empty();
                wake = wake_;
            }
            if (more && wake)
                wake();
            return ran;
        }
    }
}

bool OwnerQueue::hasPending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !items_.empty();
}

void OwnerQueue::setWake(std::function<void()> wake) {
    std::lock_guard<std::mutex> lock(mutex_);
    wake_ = std::move(wake);
}

void OwnerQueue::close() {
    std::deque<Item> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        pending.swap(items_);
    }
    for (auto& item : pending) {
        item.done.set_exception(
            std::make_exception_ptr(std::runtime_error("TBCAD is shutting down")));
    }
}

}  // namespace tbcad::mcp
