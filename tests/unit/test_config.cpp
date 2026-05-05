// Unit tests for socksdirect::Config.

#include "socksdirect/config.hpp"

#include <gtest/gtest.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace {

class ConfigFixture : public ::testing::Test {
protected:
    std::string path = "/tmp/socksdirect-test.conf";

    void write(const std::string& body) {
        std::ofstream(path) << body;
    }

    void TearDown() override {
        std::remove(path.c_str());
        // Clean up env vars we may have set.
        unsetenv("SOCKSDIRECT_MONITOR_LOG_LEVEL");
        unsetenv("SOCKSDIRECT_RDMA_DEVICE");
        unsetenv("SOCKSDIRECT_CONFIG");
    }
};

TEST_F(ConfigFixture, MissingFileReturnsDefaults) {
    auto c = socksdirect::Config::load("/nonexistent/path/socksdirect.conf");
    EXPECT_EQ("info", c.get_string("monitor", "log_level", "info"));
    EXPECT_EQ(8080, c.get_int("server", "port", 8080));
    EXPECT_FALSE(c.get_bool("debug", "verbose", false));
}

TEST_F(ConfigFixture, ReadsBasicSectionsAndKeys) {
    write(R"(
# global comment
[monitor]
log_level = debug
socket_path = /run/socksdirect/monitor.sock

[rdma]
device = mlx5_0
qp_depth = 256
fallback = rxe
)");
    auto c = socksdirect::Config::load(path);
    EXPECT_EQ("debug", c.get_string("monitor", "log_level", "info"));
    EXPECT_EQ("/run/socksdirect/monitor.sock",
              c.get_string("monitor", "socket_path", ""));
    EXPECT_EQ("mlx5_0", c.get_string("rdma", "device", "auto"));
    EXPECT_EQ(256, c.get_int("rdma", "qp_depth", 128));
}

TEST_F(ConfigFixture, EnvOverridesFile) {
    write(R"(
[monitor]
log_level = info
)");
    setenv("SOCKSDIRECT_MONITOR_LOG_LEVEL", "trace", 1);
    auto c = socksdirect::Config::load(path);
    EXPECT_EQ("trace", c.get_string("monitor", "log_level", "info"));
}

TEST_F(ConfigFixture, EnvWorksWithoutFile) {
    setenv("SOCKSDIRECT_RDMA_DEVICE", "mlx5_1", 1);
    auto c = socksdirect::Config::load("/nonexistent");
    EXPECT_EQ("mlx5_1", c.get_string("rdma", "device", "auto"));
}

TEST_F(ConfigFixture, BoolParsing) {
    write(R"(
[a]
yes_val = yes
no_val  = no
true_val = TRUE
false_val = False
one = 1
zero = 0
on = on
off = off
)");
    auto c = socksdirect::Config::load(path);
    EXPECT_TRUE(c.get_bool("a", "yes_val", false));
    EXPECT_FALSE(c.get_bool("a", "no_val", true));
    EXPECT_TRUE(c.get_bool("a", "true_val", false));
    EXPECT_FALSE(c.get_bool("a", "false_val", true));
    EXPECT_TRUE(c.get_bool("a", "one", false));
    EXPECT_FALSE(c.get_bool("a", "zero", true));
    EXPECT_TRUE(c.get_bool("a", "on", false));
    EXPECT_FALSE(c.get_bool("a", "off", true));
}

TEST_F(ConfigFixture, BadIntFallsBackAndDoesNotThrow) {
    write(R"(
[monitor]
qp_depth = not-a-number
)");
    auto c = socksdirect::Config::load(path);
    EXPECT_EQ(99, c.get_int("monitor", "qp_depth", 99));
}

TEST_F(ConfigFixture, RequireThrowsOnMissingKeys) {
    write(R"(
[monitor]
log_level = info
)");
    auto c = socksdirect::Config::load(path);
    EXPECT_NO_THROW(c.require({{"monitor", "log_level"}}));
    EXPECT_THROW(c.require({{"monitor", "nonexistent"}}), std::runtime_error);
}

TEST_F(ConfigFixture, AllPairsReturned) {
    write(R"(
[a]
x = 1
[b]
y = 2
)");
    auto c = socksdirect::Config::load(path);
    auto pairs = c.all();
    EXPECT_EQ(2u, pairs.size());
    EXPECT_EQ("1", pairs.at("a.x"));
    EXPECT_EQ("2", pairs.at("b.y"));
}

}  // namespace
