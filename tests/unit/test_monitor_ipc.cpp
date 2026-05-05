// Unit tests for socksdirect::MonitorIpc.

#include "socksdirect/monitor_ipc.hpp"

#include <gtest/gtest.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>

namespace {

using socksdirect::CtlRequest;
using socksdirect::CtlResponse;

TEST(MonitorIpc, EncodeRequestRoundTrip) {
    CtlRequest r{"status", {"json", "verbose"}};
    auto enc = socksdirect::encode_request(r);
    EXPECT_EQ('\n', enc.back());
    CtlRequest r2;
    ASSERT_TRUE(socksdirect::decode_request(enc, r2));
    EXPECT_EQ("status", r2.op);
    ASSERT_EQ(2u, r2.args.size());
    EXPECT_EQ("json", r2.args[0]);
    EXPECT_EQ("verbose", r2.args[1]);
}

TEST(MonitorIpc, EncodeResponseOkWithLines) {
    CtlResponse resp;
    resp.ok = true;
    resp.lines = {"alpha", "beta with \"quote\"", "gamma\n"};
    auto enc = socksdirect::encode_response(resp);
    CtlResponse decoded;
    ASSERT_TRUE(socksdirect::decode_response(enc, decoded));
    EXPECT_TRUE(decoded.ok);
    ASSERT_EQ(3u, decoded.lines.size());
    EXPECT_EQ("alpha", decoded.lines[0]);
    EXPECT_EQ("beta with \"quote\"", decoded.lines[1]);
    EXPECT_EQ("gamma\n", decoded.lines[2]);
}

TEST(MonitorIpc, EncodeResponseError) {
    CtlResponse resp;
    resp.ok = false;
    resp.error = "no such op";
    auto enc = socksdirect::encode_response(resp);
    CtlResponse decoded;
    ASSERT_TRUE(socksdirect::decode_response(enc, decoded));
    EXPECT_FALSE(decoded.ok);
    EXPECT_EQ("no such op", decoded.error);
    EXPECT_TRUE(decoded.lines.empty());
}

TEST(MonitorIpc, RejectsUnterminatedString) {
    CtlRequest r;
    EXPECT_FALSE(socksdirect::decode_request("{\"op\":\"abc", r));
}

TEST(MonitorIpc, AcceptsExtraWhitespace) {
    CtlRequest r;
    EXPECT_TRUE(socksdirect::decode_request(
        "{ \"op\" : \"x\" , \"args\" : [ ] }\n", r));
    EXPECT_EQ("x", r.op);
    EXPECT_TRUE(r.args.empty());
}

TEST(MonitorIpc, IgnoresUnknownKeysInRequest) {
    CtlRequest r;
    EXPECT_TRUE(socksdirect::decode_request(
        "{\"op\":\"x\",\"meta\":\"foo\",\"args\":[\"a\"],\"junk\":[\"y\"]}", r));
    EXPECT_EQ("x", r.op);
    ASSERT_EQ(1u, r.args.size());
    EXPECT_EQ("a", r.args[0]);
}

TEST(MonitorIpc, RejectsRequestMissingRequiredFields) {
    CtlRequest r;
    EXPECT_FALSE(socksdirect::decode_request("{}", r));
    EXPECT_FALSE(socksdirect::decode_request("{\"op\":\"x\"}", r));
    EXPECT_FALSE(socksdirect::decode_request("{\"args\":[]}", r));
}

TEST(MonitorIpc, EscapeAndUnescapeControlCharacters) {
    CtlRequest r{"op", {"\b\f\t\r\n\"\\"}};
    auto enc = socksdirect::encode_request(r);
    CtlRequest r2;
    ASSERT_TRUE(socksdirect::decode_request(enc, r2));
    ASSERT_EQ(1u, r2.args.size());
    EXPECT_EQ("\b\f\t\r\n\"\\", r2.args[0]);
}

TEST(MonitorIpc, UnicodeEscapeBmp) {
    // Decoder must accept "é" -> 0xC3 0xA9 (UTF-8 'é').
    CtlRequest r;
    ASSERT_TRUE(socksdirect::decode_request(
        "{\"op\":\"x\",\"args\":[\"\\u00e9\"]}", r));
    ASSERT_EQ(1u, r.args.size());
    EXPECT_EQ("\xc3\xa9", r.args[0]);
}

TEST(MonitorIpc, ReadLineAndWriteAllOverSocketpair) {
    int sv[2];
    ASSERT_EQ(0, ::socketpair(AF_UNIX, SOCK_STREAM, 0, sv));

    // Run a server in another thread that echoes a response.
    std::thread srv([&]() {
        bool eof = false;
        std::string line = socksdirect::read_line(sv[0], &eof);
        CtlRequest req;
        ASSERT_TRUE(socksdirect::decode_request(line, req));
        CtlResponse resp;
        resp.ok = true;
        resp.lines = {"got " + req.op, "with " + std::to_string(req.args.size()) + " args"};
        ASSERT_TRUE(socksdirect::write_all(sv[0], socksdirect::encode_response(resp)));
        ::close(sv[0]);
    });

    // Client writes a request, reads response.
    CtlRequest req{"status", {"a", "b", "c"}};
    ASSERT_TRUE(socksdirect::write_all(sv[1], socksdirect::encode_request(req)));
    std::string resp_line = socksdirect::read_line(sv[1]);
    CtlResponse resp;
    ASSERT_TRUE(socksdirect::decode_response(resp_line, resp));
    EXPECT_TRUE(resp.ok);
    ASSERT_EQ(2u, resp.lines.size());
    EXPECT_EQ("got status", resp.lines[0]);
    EXPECT_EQ("with 3 args", resp.lines[1]);

    srv.join();
    ::close(sv[1]);
}

TEST(MonitorIpc, ListenAndConnectUnix) {
    char tmpl[] = "/tmp/sd-ctl-test.XXXXXX";
    int tfd = ::mkstemp(tmpl);
    ASSERT_GE(tfd, 0);
    ::close(tfd);
    ::unlink(tmpl);

    int srv_fd = socksdirect::listen_unix(tmpl);
    ASSERT_GE(srv_fd, 0);

    std::thread server([&]() {
        int c = ::accept(srv_fd, nullptr, nullptr);
        if (c < 0) return;
        bool eof = false;
        std::string line = socksdirect::read_line(c, &eof);
        CtlRequest req;
        if (socksdirect::decode_request(line, req)) {
            CtlResponse resp;
            resp.ok = true;
            resp.lines = {"echo " + req.op};
            socksdirect::write_all(c, socksdirect::encode_response(resp));
        }
        ::close(c);
    });

    int cfd = socksdirect::connect_unix(tmpl);
    ASSERT_GE(cfd, 0);
    socksdirect::write_all(cfd, socksdirect::encode_request({"ping", {}}));
    std::string line = socksdirect::read_line(cfd);
    CtlResponse resp;
    ASSERT_TRUE(socksdirect::decode_response(line, resp));
    EXPECT_TRUE(resp.ok);
    ASSERT_EQ(1u, resp.lines.size());
    EXPECT_EQ("echo ping", resp.lines[0]);

    ::close(cfd);
    server.join();
    ::close(srv_fd);
    ::unlink(tmpl);
}

TEST(MonitorIpc, ConnectFailsWhenSocketAbsent) {
    int fd = socksdirect::connect_unix("/tmp/socksdirect-nope-XXXXX-zz");
    EXPECT_EQ(-1, fd);
    EXPECT_NE(0, errno);
}

}  // namespace
