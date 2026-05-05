// Unit tests for the C timing helpers in common/helper.c.
//
// These functions are used by every benchmark to convert TSC reads into
// nanoseconds. The TSC calibration runs a tight loop on the calling
// thread; on a CI runner under load the calibration variance can be high,
// so we only check loose bounds (no negative numbers, sane order of
// magnitude).

extern "C" {
#include "common/helper.h"
}

#include <gtest/gtest.h>
#include <thread>
#include <chrono>

namespace {

TEST(HelperTiming, GettidIsPositiveAndStableWithinThread) {
    pid_t a = gettid();
    pid_t b = gettid();
    EXPECT_GT(a, 0);
    EXPECT_EQ(a, b);
}

TEST(HelperTiming, GettidDiffersAcrossThreads) {
    pid_t main_tid = gettid();
    pid_t other_tid = 0;
    std::thread th([&] { other_tid = gettid(); });
    th.join();
    EXPECT_NE(main_tid, other_tid);
}

TEST(HelperTiming, TimingMeasuresMonotonicallyForward) {
    TimingInit();
    InitRdtsc();
    TimingBegin();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    unsigned long ns = TimingEnd();
    // Expect at least 5 ms (50% of nominal sleep, very generous bound to
    // accommodate jittery CI), and at most 2 s (catches obvious unit bugs).
    EXPECT_GT(ns, 5000000UL);
    EXPECT_LT(ns, 2000000000UL);
}

}  // namespace
