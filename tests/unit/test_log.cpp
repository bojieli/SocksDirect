// Unit tests for socksdirect::Logger.
//
// The logger writes through a FILE*. To assert on output without touching
// the real stderr, we redirect the sink to an in-memory FILE* opened via
// open_memstream and read it back.

#include "socksdirect/log.hpp"

#include <gtest/gtest.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {

// open_memstream-backed FILE* that auto-frees on destruction.
struct MemSink {
    char* buf = nullptr;
    size_t sz = 0;
    FILE* f = nullptr;

    MemSink() {
        f = ::open_memstream(&buf, &sz);
        EXPECT_NE(nullptr, f);
    }
    ~MemSink() {
        if (f) std::fclose(f);
        std::free(buf);
    }
    std::string contents() {
        std::fflush(f);
        return std::string(buf, sz);
    }
};

class LoggerFixture : public ::testing::Test {
protected:
    void SetUp() override {
        // Reset to a known state. We work with a fresh instance via a
        // local Logger, not the global one — the global one is used by
        // other tests in the binary.
        lg.set_level(::socksdirect::kLogTrace);
        lg.set_sink_FILE(sink.f);
    }

    socksdirect::Logger lg;
    MemSink sink;
};

TEST_F(LoggerFixture, ParsesLevelStrings) {
    using namespace socksdirect;
    EXPECT_EQ(kLogTrace, parse_level("trace"));
    EXPECT_EQ(kLogDebug, parse_level("debug"));
    EXPECT_EQ(kLogInfo,  parse_level("info"));
    EXPECT_EQ(kLogWarn,  parse_level("warn"));
    EXPECT_EQ(kLogWarn,  parse_level("warning"));
    EXPECT_EQ(kLogError, parse_level("ERROR"));
    EXPECT_EQ(kLogOff,   parse_level("off"));
    // Unknown -> fallback.
    EXPECT_EQ(kLogInfo,  parse_level("zorp", kLogInfo));
    // First-letter alias.
    EXPECT_EQ(kLogDebug, parse_level("d"));
    EXPECT_EQ(kLogInfo,  parse_level(nullptr, kLogInfo));
}

TEST_F(LoggerFixture, RespectsLevel) {
    lg.set_level(socksdirect::kLogWarn);
    lg.log(socksdirect::kLogInfo, __FILE__, __LINE__, "should-not-appear");
    lg.log(socksdirect::kLogWarn, __FILE__, __LINE__, "should-appear");
    auto out = sink.contents();
    EXPECT_EQ(std::string::npos, out.find("should-not-appear")) << out;
    EXPECT_NE(std::string::npos, out.find("should-appear")) << out;
}

TEST_F(LoggerFixture, EmitsExpectedFields) {
    lg.set_level(socksdirect::kLogTrace);
    lg.log(socksdirect::kLogInfo, "foo/bar.cpp", 42, "hello %s %d", "world", 7);
    auto out = sink.contents();
    EXPECT_NE(std::string::npos, out.find("info"));
    EXPECT_NE(std::string::npos, out.find("bar.cpp:42"));
    EXPECT_NE(std::string::npos, out.find("hello world 7"));
    EXPECT_NE(std::string::npos, out.find("pid="));
    EXPECT_NE(std::string::npos, out.find("tid="));
    EXPECT_NE(std::string::npos, out.find("Z "));   // ISO-8601 trailing Z
}

TEST_F(LoggerFixture, TruncatesLongMessage) {
    std::string huge(4096, 'x');
    lg.log(socksdirect::kLogInfo, __FILE__, __LINE__, "%s", huge.c_str());
    auto out = sink.contents();
    EXPECT_NE(std::string::npos, out.find("...")) << "expected truncation marker";
    // Body cap is 1024; full 4096 must NOT be present.
    EXPECT_EQ(std::string::npos, out.find(std::string(1500, 'x')));
}

TEST_F(LoggerFixture, ConcurrentWritesAreSerialized) {
    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;
    std::vector<std::thread> ts;
    std::atomic<int> ready{0};
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&, t]() {
            ++ready;
            while (ready.load() < kThreads) std::this_thread::yield();
            for (int i = 0; i < kPerThread; ++i) {
                lg.log(socksdirect::kLogInfo, __FILE__, __LINE__, "T%d-%d", t, i);
            }
        });
    }
    for (auto& th : ts) th.join();
    auto out = sink.contents();

    // Every line must be well-formed (start with 4-digit year, contain
    // a "T<n>-<n>" body). If a write tore another, we'd see one of these
    // patterns split across a newline.
    int lines = 0;
    for (size_t pos = 0; pos < out.size();) {
        size_t eol = out.find('\n', pos);
        if (eol == std::string::npos) eol = out.size();
        std::string line = out.substr(pos, eol - pos);
        if (!line.empty()) {
            ++lines;
            EXPECT_GE(line.size(), 8u);
            // 4-digit year prefix.
            for (int i = 0; i < 4; ++i)
                EXPECT_TRUE(::isdigit(static_cast<unsigned char>(line[i]))) << line;
            EXPECT_NE(std::string::npos, line.find("T")) << line;
        }
        pos = eol + 1;
    }
    EXPECT_EQ(kThreads * kPerThread, lines);
}

TEST_F(LoggerFixture, SetSinkFileOnDiskRoundtrips) {
    char tmp[] = "/tmp/socksdirect-log-test.XXXXXX";
    int fd = ::mkstemp(tmp);
    ASSERT_GE(fd, 0);
    ::close(fd);

    // Switch to file sink, write something, switch back, read the file.
    EXPECT_TRUE(lg.set_sink_file(tmp));
    EXPECT_EQ(std::string(tmp), lg.sink_path());
    lg.log(socksdirect::kLogInfo, __FILE__, __LINE__, "into-file");
    lg.set_sink_FILE(sink.f);  // restore for fixture cleanup

    FILE* rfh = std::fopen(tmp, "r");
    ASSERT_NE(nullptr, rfh);
    char buf[2048] = {0};
    size_t n = std::fread(buf, 1, sizeof(buf) - 1, rfh);
    std::fclose(rfh);
    std::remove(tmp);
    EXPECT_GT(n, 0u);
    EXPECT_NE(std::string::npos, std::string(buf).find("into-file"));
}

TEST_F(LoggerFixture, FailedSinkFallsBackToStderrWithoutCrashing) {
    EXPECT_FALSE(lg.set_sink_file("/nonexistent-dir-zzzzz/log.log"));
    // Logging after a failed sink open must not crash.
    lg.log(socksdirect::kLogInfo, __FILE__, __LINE__, "after-failed-sink");
}

TEST_F(LoggerFixture, MacroSkipsFormattingBelowLevel) {
    // Set a level that filters out info; if the macro fails to short-circuit,
    // a side-effecting expression in the args would run.
    lg.set_level(socksdirect::kLogError);
    int side_effect = 0;
    auto bump = [&]() { ++side_effect; return 1; };
    // Bind global instance to our mem sink for this test only.
    auto& gl = socksdirect::Logger::instance();
    auto saved_level = gl.level();
    gl.set_level(socksdirect::kLogError);
    gl.set_sink_FILE(sink.f);
    LOG_INFO("never-formatted %d", bump());
    gl.set_level(saved_level);
    gl.set_sink_FILE(stderr);
    EXPECT_EQ(0, side_effect);
}

}  // namespace
