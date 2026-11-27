#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace bv::player {

// Bounded blocking queue with cooperative abort. Used between the demux, decode and render
// threads so each stage applies back-pressure instead of buffering without limit.
template <class T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

    // Blocks while full. Returns false if aborted.
    bool push(T value) {
        std::unique_lock lock(mutex_);
        notFull_.wait(lock, [&] { return aborted_ || items_.size() < capacity_; });
        if (aborted_) return false;
        items_.push_back(std::move(value));
        notEmpty_.notify_one();
        return true;
    }

    // Blocks while empty. Returns nullopt if aborted.
    std::optional<T> pop() {
        std::unique_lock lock(mutex_);
        notEmpty_.wait(lock, [&] { return aborted_ || !items_.empty(); });
        if (aborted_) return std::nullopt;
        return take(lock);
    }

    // Waits at most `timeout`. Returns nullopt on timeout or abort.
    template <class Duration>
    std::optional<T> popFor(Duration timeout) {
        std::unique_lock lock(mutex_);
        if (!notEmpty_.wait_for(lock, timeout, [&] { return aborted_ || !items_.empty(); }) || aborted_) {
            return std::nullopt;
        }
        return take(lock);
    }

    // Non-blocking access to the head, for schedulers that peek before committing.
    template <class Fn>
    auto withFront(Fn&& fn) -> decltype(fn(static_cast<T*>(nullptr))) {
        std::lock_guard lock(mutex_);
        return fn(items_.empty() ? nullptr : &items_.front());
    }

    std::optional<T> tryPop() {
        std::unique_lock lock(mutex_);
        if (items_.empty()) return std::nullopt;
        return take(lock);
    }

    void clear() {
        std::lock_guard lock(mutex_);
        items_.clear();
        notFull_.notify_all();
    }

    void abort() {
        std::lock_guard lock(mutex_);
        aborted_ = true;
        notFull_.notify_all();
        notEmpty_.notify_all();
    }

    void reset() {
        std::lock_guard lock(mutex_);
        items_.clear();
        aborted_ = false;
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard lock(mutex_);
        return items_.size();
    }

private:
    T take(std::unique_lock<std::mutex>&) {
        T v = std::move(items_.front());
        items_.pop_front();
        notFull_.notify_one();
        return v;
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable notFull_, notEmpty_;
    std::deque<T> items_;
    bool aborted_ = false;
};

} // namespace bv::player
