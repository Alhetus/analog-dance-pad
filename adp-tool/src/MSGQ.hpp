#ifndef	MSGQ_HPP_
#define	MSGQ_HPP_

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

// A single inbound message from a WebSocket client. Carried by value through
// the queue below; no manual new/delete across the thread boundary.
class QueueMessage {
public:
    QueueMessage() = default;
    explicit QueueMessage(std::string d) : data(std::move(d)) {}

    std::string data;
};

// Thread-safe multi-producer / single-consumer queue.
//
// Works for any movable T (no pointer-sentinel requirement). Consumers can
// drain without blocking (tryPop) or block until work arrives (waitPop).
// stop() wakes every blocked waiter so a consumer loop can exit cleanly on
// shutdown; once stopped and drained, waitPop() returns std::nullopt.
template<class T>
class MSGQ {
public:
    MSGQ() = default;
    ~MSGQ() = default;

    MSGQ(const MSGQ&) = delete;
    MSGQ& operator=(const MSGQ&) = delete;

    void push(T item) {
        {
            std::lock_guard<std::mutex> lock(q_mtx);
            queue.push_back(std::move(item));
        }
        q_cv.notify_one();
    }

    // Non-blocking: returns the next item if one is ready, else std::nullopt.
    std::optional<T> tryPop() {
        std::lock_guard<std::mutex> lock(q_mtx);
        if (queue.empty()) {
            return std::nullopt;
        }
        T item = std::move(queue.front());
        queue.pop_front();
        return item;
    }

    // Blocks until an item is available or the queue is stopped. Returns
    // std::nullopt only when stopped and no items remain.
    std::optional<T> waitPop() {
        std::unique_lock<std::mutex> lock(q_mtx);
        q_cv.wait(lock, [this] { return stopped || !queue.empty(); });
        if (queue.empty()) {
            return std::nullopt;
        }
        T item = std::move(queue.front());
        queue.pop_front();
        return item;
    }

    // Signals shutdown and wakes all waiters.
    void stop() {
        {
            std::lock_guard<std::mutex> lock(q_mtx);
            stopped = true;
        }
        q_cv.notify_all();
    }

    bool isStopped() const {
        std::lock_guard<std::mutex> lock(q_mtx);
        return stopped;
    }

private:
    std::deque<T> queue;
    mutable std::mutex q_mtx;
    std::condition_variable q_cv;
    bool stopped = false;
};

#endif
