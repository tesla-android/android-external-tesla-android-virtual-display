#pragma once

#include <condition_variable>
#include <chrono>
#include <mutex>

#include "minicap.hpp"

class FrameWaiter : public Minicap::FrameAvailableListener {
public:
    FrameWaiter(): mTimeout(std::chrono::milliseconds(100)), 
      mPendingFrames(0),
      mStopped(false) {}
    int waitForFrame();
    void reportExtraConsumption(int count);
    void onFrameAvailable();
    void stop();
    bool isStopped();

private:
    std::mutex mMutex;
    std::condition_variable mCondition;
    std::chrono::milliseconds mTimeout;
    int mPendingFrames;
    bool mStopped;
};
