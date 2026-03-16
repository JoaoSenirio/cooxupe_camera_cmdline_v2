#ifndef THREAD_QUEUE_H
#define THREAD_QUEUE_H

#include <condition_variable>
#include <mutex>
#include <queue>

template <typename T>
class ThreadQueue {
public:
    bool push(const T& item) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return false;
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

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    bool closed_ = false;
};

#endif
