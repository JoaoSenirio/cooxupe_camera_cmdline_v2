#ifndef THREAD_QUEUE_H
#define THREAD_QUEUE_H

#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <mutex>
#include <queue>

template <typename T>
class ThreadQueue {
public:
    explicit ThreadQueue(std::size_t max_size = 0) : max_size_(max_size) {}

    bool push(const T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return false;
        }
        if (max_size_ > 0 && queue_.size() >= max_size_) {
            return false;
        }
        queue_.push(item);
        cv_.notify_one();
        return true;
    }

    bool push_for(const T& item, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (closed_) {
            return false;
        }
        if (max_size_ > 0 && queue_.size() >= max_size_) {
            const bool has_space = cv_.wait_for(lock, timeout, [this]() {
                return closed_ || queue_.size() < max_size_;
            });
            if (!has_space || closed_ || (max_size_ > 0 && queue_.size() >= max_size_)) {
                return false;
            }
        }

        queue_.push(item);
        cv_.notify_one();
        return true;
    }

    bool pop(T* out) {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]() { return closed_ || !queue_.empty(); });
        if (queue_.empty()) {
            return false;
        }
        *out = queue_.front();
        queue_.pop();
        cv_.notify_all();
        return true;
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        cv_.notify_all();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    std::size_t capacity() const {
        return max_size_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    std::size_t max_size_ = 0;
    bool closed_ = false;
};

#endif
