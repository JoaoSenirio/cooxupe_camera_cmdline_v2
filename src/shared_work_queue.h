#ifndef SHARED_WORK_QUEUE_H
#define SHARED_WORK_QUEUE_H

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "types.h"

enum class SharedWorkConsumer : std::uint8_t {
    Save = 0,
    Stream = 1,
};

class SharedWorkQueue {
public:
    static constexpr std::uint8_t kConsumerSaveMask = 0x01;
    static constexpr std::uint8_t kConsumerStreamMask = 0x02;

    struct Lease {
        SharedWorkConsumer consumer = SharedWorkConsumer::Save;
        std::uint64_t sequence = 0;
        std::shared_ptr<const WorkItem> item;
    };

    explicit SharedWorkQueue(std::size_t capacity,
                             std::uint8_t consumer_mask = kConsumerSaveMask | kConsumerStreamMask)
        : capacity_(capacity > 0 ? capacity : 1),
          consumer_mask_(consumer_mask) {}

    bool publish(WorkItem item, std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock<std::mutex> lock(mutex_);
        while (!failed_ && !closed_ && entries_.size() >= capacity_) {
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                if (entries_.size() >= capacity_) {
                    return false;
                }
            }
        }
        if (failed_ || closed_) {
            return false;
        }

        Entry entry;
        entry.sequence = next_publish_sequence_++;
        entry.pending_mask = consumer_mask_;
        entry.item = std::make_shared<WorkItem>(std::move(item));
        entries_.push_back(std::move(entry));
        cv_.notify_all();
        return true;
    }

    bool pop(SharedWorkConsumer consumer, Lease* out) {
        if (out == nullptr) {
            return false;
        }

        const std::size_t consumer_index = ConsumerIndex(consumer);
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [&]() {
            return failed_ || HasEntryLocked(consumer_index) || (closed_ && !HasEntryLocked(consumer_index));
        });

        if (failed_) {
            return false;
        }
        if (!HasEntryLocked(consumer_index)) {
            return false;
        }

        Entry* entry = FindEntryLocked(next_sequence_[consumer_index]);
        if (entry == nullptr || entry->item == nullptr) {
            return false;
        }

        out->consumer = consumer;
        out->sequence = entry->sequence;
        out->item = entry->item;
        ++next_sequence_[consumer_index];
        return true;
    }

    void ack(const Lease& lease, bool success, const std::string& failure_reason = std::string()) {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry* entry = FindEntryLocked(lease.sequence);
        if (entry == nullptr) {
            if (!success) {
                failed_ = true;
                failure_reason_ = failure_reason;
                cv_.notify_all();
            }
            return;
        }

        if (!success) {
            failed_ = true;
            failure_reason_ = failure_reason;
            cv_.notify_all();
            return;
        }

        entry->pending_mask &= static_cast<std::uint8_t>(~ConsumerMask(lease.consumer));
        RetireFrontLocked();
        cv_.notify_all();
    }

    void close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        cv_.notify_all();
    }

    bool failed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return failed_;
    }

    std::string failure_reason() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return failure_reason_;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

private:
    struct Entry {
        std::uint64_t sequence = 0;
        std::uint8_t pending_mask = 0;
        std::shared_ptr<WorkItem> item;
    };

    static std::size_t ConsumerIndex(SharedWorkConsumer consumer) {
        return consumer == SharedWorkConsumer::Save ? 0U : 1U;
    }

    static std::uint8_t ConsumerMask(SharedWorkConsumer consumer) {
        return consumer == SharedWorkConsumer::Save ? kConsumerSaveMask : kConsumerStreamMask;
    }

    bool HasEntryLocked(std::size_t consumer_index) const {
        return FindEntryLocked(next_sequence_[consumer_index]) != nullptr;
    }

    Entry* FindEntryLocked(std::uint64_t sequence) {
        for (Entry& entry : entries_) {
            if (entry.sequence == sequence) {
                return &entry;
            }
        }
        return nullptr;
    }

    const Entry* FindEntryLocked(std::uint64_t sequence) const {
        for (const Entry& entry : entries_) {
            if (entry.sequence == sequence) {
                return &entry;
            }
        }
        return nullptr;
    }

    void RetireFrontLocked() {
        while (!entries_.empty() && entries_.front().pending_mask == 0) {
            entries_.pop_front();
        }
    }

    const std::size_t capacity_;
    const std::uint8_t consumer_mask_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Entry> entries_;
    std::array<std::uint64_t, 2> next_sequence_{{1, 1}};
    std::uint64_t next_publish_sequence_ = 1;
    bool closed_ = false;
    bool failed_ = false;
    std::string failure_reason_;
};

#endif
