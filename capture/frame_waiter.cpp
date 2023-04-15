#include "frame_waiter.h"

int
FrameWaiter::waitForFrame() {
    std::unique_lock<std::mutex> lock(mMutex);

    while (!mStopped) {
        if (mCondition.wait_for(lock, mTimeout, [this] { return mPendingFrames > 0; })) {
            return mPendingFrames--;
        }
    }

    return 0;
}

void
FrameWaiter::reportExtraConsumption(int count) {
    std::unique_lock<std::mutex> lock(mMutex);
    mPendingFrames -= count;
}

void
FrameWaiter::onFrameAvailable() {
    std::unique_lock<std::mutex> lock(mMutex);
    mPendingFrames += 1;
    mCondition.notify_one();
}

void
FrameWaiter::stop() {
    mStopped = true;
}

bool
FrameWaiter::isStopped() {
    return mStopped;
}
