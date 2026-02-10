#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>

template <typename T>
class ThreadSafeQueue {
public:
    void push(const T& value);
    T pop();
    bool try_pop(T& value);

private:
    std::deque<T> queue_;
    std::mutex mutex_;
    std::condition_variable cond_;
};
