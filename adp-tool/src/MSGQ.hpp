#ifndef MSGQ_HPP_
#define MSGQ_HPP_

#include <deque>
#include <mutex>
#include <optional>
#include <string>

// A single inbound message from a WebSocket client. Carried by value through
// the queue below; no manual new/delete across the thread boundary.
class QueueMessage
{
  public:
	QueueMessage() = default;
	explicit QueueMessage(std::string d, std::string s = "") : data(std::move(d)), sender(std::move(s)) {}

	std::string data;
	// Connection id of the sender (ix ConnectionState id, unique per
	// connection, never reused), so replies meant for it reach only it.
	std::string sender;
};

// Thread-safe multi-producer / single-consumer queue, drained without
// blocking by the device-I/O loop.
template <class T> class MSGQ
{
  public:
	MSGQ() = default;

	MSGQ(const MSGQ&) = delete;
	MSGQ& operator=(const MSGQ&) = delete;

	void push(T item)
	{
		std::lock_guard<std::mutex> lock(q_mtx);
		queue.push_back(std::move(item));
	}

	// Non-blocking: returns the next item if one is ready, else std::nullopt.
	std::optional<T> tryPop()
	{
		std::lock_guard<std::mutex> lock(q_mtx);
		if (queue.empty())
			return std::nullopt;
		T item = std::move(queue.front());
		queue.pop_front();
		return item;
	}

  private:
	std::deque<T> queue;
	std::mutex q_mtx;
};

#endif
