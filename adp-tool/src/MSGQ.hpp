#ifndef	MSGQ_HPP_
#define	MSGQ_HPP_

#include <string>
#include <list>
#include <mutex>

class QueueMessage {
public:
    QueueMessage() = default;
    ~QueueMessage() = default;

    std::string data;
};

template<class T>
class MSGQ {
public:
    MSGQ() = default;
    ~MSGQ() = default;

    void addElem(T item) {
        std::unique_lock<std::mutex> lock(q_mtx);

        this->queue.push_back(item);
    }

    T popElem() {
        std::unique_lock<std::mutex> lock(q_mtx);

        if (this->queue.size() == 0) {
            return nullptr;
        }

        T item = queue.front();
        queue.pop_front();
        return item;
    }

private:
    std::list<T> queue;
    std::mutex q_mtx;
};

#endif
