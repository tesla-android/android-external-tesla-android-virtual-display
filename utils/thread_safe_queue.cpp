#include "utils/thread_safe_queue.h"
#include "encode/frame.h"
#include "encode/m2m.h"

template <typename T>
void ThreadSafeQueue<T>::push(const T& value) {
    std::unique_lock<std::mutex> lock(mutex_);
    queue_.push_back(value);
    lock.unlock();
    cond_.notify_one();
}

template <typename T>
T ThreadSafeQueue<T>::pop() {
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait(lock, [&] { return !queue_.empty(); });
    T value = queue_.front();
    queue_.pop_front();
    return value;
}

template <typename T>
bool ThreadSafeQueue<T>::try_pop(T& value) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (queue_.empty()) {
        return false;
    }
    value = queue_.front();
    queue_.pop_front();
    return true;
}

// Explicit template instantiation each type used with ThreadSafeQueue
template class ThreadSafeQueue<us_frame_s>;
