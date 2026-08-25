#include <algorithm>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "../src/App.h"

namespace {

struct ClientResult {
    int error = 0;
    std::string response;
};

ClientResult exchange(int port, std::string_view request,
                      std::chrono::milliseconds readDelay = {},
                      int receiveBufferBytes = 0) {
    ClientResult result;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        result.error = errno;
        return result;
    }

    const timeval timeout{5, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0) {
        result.error = errno;
        close(fd);
        return result;
    }
    if (receiveBufferBytes &&
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receiveBufferBytes,
                   sizeof(receiveBufferBytes)) != 0) {
        result.error = errno;
        close(fd);
        return result;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
        0) {
        result.error = errno;
        close(fd);
        return result;
    }

    std::size_t sent = 0;
    while (sent < request.size()) {
        ssize_t written =
            send(fd, request.data() + sent, request.size() - sent, 0);
        if (written <= 0) {
            result.error = written < 0 ? errno : EIO;
            close(fd);
            return result;
        }
        sent += static_cast<std::size_t>(written);
    }

    if (readDelay.count()) {
        std::this_thread::sleep_for(readDelay);
    }

    char buffer[1024];
    for (;;) {
        ssize_t received = recv(fd, buffer, sizeof(buffer), 0);
        if (received == 0) {
            break;
        }
        if (received < 0) {
            result.error = errno;
            break;
        }
        result.response.append(buffer, static_cast<std::size_t>(received));
    }
    close(fd);
    return result;
}

void requestTrailersPrecedeBodyEnd() {
    std::vector<std::string> events;
    std::vector<std::pair<std::string, std::string>> trailers;
    std::string body;
    us_listen_socket_t *listenSocket = nullptr;
    ClientResult clientResult;
    std::thread client;

    {
        uWS::App app({}, uWS::HttpContextOptions{true});
        app.any("/*", [&](auto *response, auto */*request*/) {
            events.emplace_back("head");
            response->onAborted([]() {});
            response->onRequestTrailers(
                [&](uWS::HttpRequestTrailers *requestTrailers) {
                    events.emplace_back("trailers");
                    for (auto [name, value] : *requestTrailers) {
                        trailers.emplace_back(name, value);
                    }
                });
            response->onDataV2(
                [&, response](std::string_view chunk, uint64_t remaining) {
                    if (remaining) {
                        events.emplace_back("body");
                        body.append(chunk);
                        return;
                    }
                    events.emplace_back("end");
                    response->end("ok");
                    us_listen_socket_close(0, listenSocket);
                });
        }).listen("127.0.0.1", 0, [&](auto *token) {
            assert(token);
            listenSocket = token;
            int port = us_socket_local_port(
                0, reinterpret_cast<us_socket_t *>(listenSocket));
            client = std::thread([&clientResult, port]() {
                clientResult = exchange(
                    port,
                    "POST /trailers HTTP/1.1\r\n"
                    "Host: example.test\r\n"
                    "Connection: close\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n"
                    "3\r\nabc\r\n0\r\n"
                    "X-Checksum: value\r\n"
                    "X-Extra:\tsecond \t\r\n\r\n");
            });
        }).run();
    }

    client.join();
    assert(clientResult.error == 0);
    assert(clientResult.response.find("HTTP/1.1 200 OK") == 0);
    assert(clientResult.response.ends_with("\r\n\r\nok"));
    assert(body == "abc");
    assert(trailers.size() == 2);
    assert(trailers[0] ==
           std::make_pair(std::string("x-checksum"), std::string("value")));
    assert(trailers[1] ==
           std::make_pair(std::string("x-extra"), std::string("second")));

    auto trailerEvent = std::find(events.begin(), events.end(), "trailers");
    auto endEvent = std::find(events.begin(), events.end(), "end");
    assert(events.front() == "head");
    assert(trailerEvent != events.end());
    assert(endEvent != events.end());
    assert(trailerEvent < endEvent);
}

void nonTrailerRequestReleasesHandlerAtBodyEnd() {
    us_listen_socket_t *listenSocket = nullptr;
    ClientResult clientResult;
    std::thread client;
    std::weak_ptr<int> trailerHandlerOwner;
    bool releaseChecked = false;

    {
        uWS::App app({}, uWS::HttpContextOptions{true});
        app.any("/*", [&](auto *response, auto */*request*/) {
            response->onAborted([]() {});

            auto owner = std::make_shared<int>(1);
            trailerHandlerOwner = owner;
            response->onRequestTrailers(
                [owner = std::move(owner)](uWS::HttpRequestTrailers *) {
                    assert(false && "non-chunked request delivered trailers");
                });
            response->onDataV2(
                [&, response](std::string_view, uint64_t remaining) {
                    if (remaining) {
                        return;
                    }
                    response->end("ok");
                    uWS::Loop::get()->defer([&]() {
                        releaseChecked = trailerHandlerOwner.expired();
                        us_listen_socket_close(0, listenSocket);
                    });
                });
        }).listen("127.0.0.1", 0, [&](auto *token) {
            assert(token);
            listenSocket = token;
            int port = us_socket_local_port(
                0, reinterpret_cast<us_socket_t *>(listenSocket));
            client = std::thread([&clientResult, port]() {
                clientResult = exchange(
                    port,
                    "POST /no-trailers HTTP/1.1\r\n"
                    "Host: example.test\r\n"
                    "Connection: close\r\n"
                    "Content-Length: 3\r\n\r\nabc");
            });
        }).run();
    }

    client.join();
    assert(clientResult.error == 0);
    assert(clientResult.response.find("HTTP/1.1 200 OK") == 0);
    assert(clientResult.response.ends_with("\r\n\r\nok"));
    assert(releaseChecked);
    assert(trailerHandlerOwner.expired());
}

void nonBufferingChunkWriteRetriesExactSuffix() {
    constexpr std::size_t payloadBytes = 4U * 1024U * 1024U;
    const std::string payload(payloadBytes, 'x');
    us_listen_socket_t *listenSocket = nullptr;
    ClientResult clientResult;
    std::thread client;
    std::size_t consumed = 0;
    std::size_t blockedCalls = 0;
    unsigned int maxProviderBuffer = 0;
    bool prematureRetryBlocked = false;
    bool checkMismatchedRetry = false;
    bool mismatchedRetryRejected = false;

    {
        uWS::App app;
        app.any("/*", [&](auto *response, auto */*request*/) {
            response->onAborted([]() { assert(false && "stream aborted"); });
            auto drive = [&, response]() {
                if (consumed == payload.size()) {
                    response->end();
                    us_listen_socket_close(0, listenSocket);
                    return true;
                }

                const std::string_view remaining =
                    std::string_view(payload).substr(consumed);
                if (checkMismatchedRetry && remaining.size() > 1U) {
                    const auto rejected =
                        response->tryWriteChunk(remaining.substr(1U));
                    mismatchedRetryRejected = !rejected.valid &&
                                              rejected.consumed == 0U &&
                                              !rejected.blocked;
                    checkMismatchedRetry = false;
                }
                const auto written = response->tryWriteChunk(remaining);
                assert(written.valid);
                assert(written.consumed <= remaining.size());
                consumed += written.consumed;
                maxProviderBuffer =
                    std::max(maxProviderBuffer, written.bufferedBytes);
                if (written.blocked) {
                    ++blockedCalls;
                    const auto prematureFinal = response->tryEnd("invalid");
                    assert(!prematureFinal.first && !prematureFinal.second);
                    const auto prematureTrailers =
                        response->endChunkedWithTrailers("x-result: early\r\n");
                    assert(!prematureTrailers.valid);
                    if (!prematureRetryBlocked) {
                        const auto premature =
                            response->tryWriteChunk(
                                std::string_view(payload).substr(consumed));
                        prematureRetryBlocked = premature.valid &&
                                                premature.blocked &&
                                                premature.consumed == 0U;
                    }
                    if (consumed < payload.size() &&
                        !mismatchedRetryRejected) {
                        checkMismatchedRetry = true;
                    }
                    return false;
                }
                assert(consumed == payload.size());
                response->end();
                us_listen_socket_close(0, listenSocket);
                return true;
            };
            response->onWritable(
                [drive](uintmax_t) mutable { return drive(); });
            (void) drive();
        }).listen("127.0.0.1", 0, [&](auto *token) {
            assert(token);
            listenSocket = token;
            int port = us_socket_local_port(
                0, reinterpret_cast<us_socket_t *>(listenSocket));
            client = std::thread([&clientResult, port]() {
                clientResult = exchange(
                    port,
                    "GET /stream HTTP/1.1\r\n"
                    "Host: example.test\r\n"
                    "Connection: close\r\n\r\n",
                    std::chrono::milliseconds(100), 4096);
            });
        }).run();
    }

    client.join();
    assert(clientResult.error == 0);
    assert(consumed == payload.size());
    assert(blockedCalls != 0);
    assert(prematureRetryBlocked);
    assert(mismatchedRetryRejected);
    assert(maxProviderBuffer < 1024U);

    const std::size_t body = clientResult.response.find("\r\n\r\n");
    assert(body != std::string::npos);
    const std::string expected =
        "400000\r\n" + payload + "\r\n0\r\n\r\n";
    assert(clientResult.response.substr(body + 4U) == expected);
}

void chunkedTrailerFinalCopiesBoundedInput() {
    constexpr std::size_t trailerValueBytes = 4U * 1024U * 1024U;
    std::string trailerFields = "x-large: ";
    trailerFields.append(trailerValueBytes, 'x');
    trailerFields.append("\r\n");
    const std::string expectedTrailerFields = trailerFields;
    us_listen_socket_t *listenSocket = nullptr;
    ClientResult clientResult;
    std::thread client;
    uWS::HttpChunkTrailerEndResult ended{};

    {
        uWS::App app;
        app.any("/*", [&](auto *response, auto */*request*/) {
            response->onAborted(
                []() { assert(false && "trailer response aborted"); });
            response->writeHeader("Connection", "close");
            response->beginWrite();
            ended = response->endChunkedWithTrailers(trailerFields, true);
            assert(ended.valid);
            assert(ended.bufferedBytes <= trailerFields.size() + 5U);
            std::fill(trailerFields.begin(), trailerFields.end(), 'z');
            us_listen_socket_close(0, listenSocket);
        }).listen("127.0.0.1", 0, [&](auto *token) {
            assert(token);
            listenSocket = token;
            int port = us_socket_local_port(
                0, reinterpret_cast<us_socket_t *>(listenSocket));
            client = std::thread([&clientResult, port]() {
                clientResult = exchange(
                    port,
                    "GET /trailers HTTP/1.1\r\n"
                    "Host: example.test\r\n"
                    "Connection: close\r\n\r\n",
                    std::chrono::milliseconds(100), 4096);
            });
        }).run();
    }

    client.join();
    assert(clientResult.error == 0);
    assert(ended.bufferedBytes != 0U);
    const std::size_t body = clientResult.response.find("\r\n\r\n");
    assert(body != std::string::npos);
    assert(clientResult.response.substr(body + 4U) ==
           "0\r\n" + expectedTrailerFields + "\r\n");
}

void chunkedTrailerFinalSupportsTrailerOnlyAndKeepAliveReset() {
    us_listen_socket_t *listenSocket = nullptr;
    ClientResult clientResult;
    std::thread client;
    unsigned int requests = 0U;

    {
        uWS::App app;
        app.any("/*", [&](auto *response, auto */*request*/) {
            response->onAborted(
                []() { assert(false && "trailer pipeline aborted"); });
            response->beginWrite();
            const auto empty = response->endChunkedWithTrailers({});
            assert(!empty.valid && !response->hasResponded());
            const auto noFields =
                response->endChunkedWithTrailers("\r\n");
            assert(!noFields.valid && !response->hasResponded());
            const auto malformed =
                response->endChunkedWithTrailers("x-result: missing-crlf");
            assert(!malformed.valid && !response->hasResponded());
            const auto requestIndex = requests++;
            if (requestIndex == 0U) {
                const auto written = response->tryWriteChunk("one");
                assert(written.valid && !written.blocked);
                assert(written.consumed == 3U);
            }
            const std::string_view fields =
                requestIndex == 0U ? "x-result: one\r\n"
                                   : "x-result: two\r\n";
            const auto ended = response->endChunkedWithTrailers(fields);
            assert(ended.valid && response->hasResponded());
            if (requests == 2U) {
                us_listen_socket_close(0, listenSocket);
            }
        }).listen("127.0.0.1", 0, [&](auto *token) {
            assert(token);
            listenSocket = token;
            int port = us_socket_local_port(
                0, reinterpret_cast<us_socket_t *>(listenSocket));
            client = std::thread([&clientResult, port]() {
                clientResult = exchange(
                    port,
                    "GET /one HTTP/1.1\r\nHost: example.test\r\n\r\n"
                    "GET /two HTTP/1.1\r\nHost: example.test\r\n"
                    "Connection: close\r\n\r\n");
            });
        }).run();
    }

    client.join();
    assert(clientResult.error == 0);
    assert(requests == 2U);
    assert(clientResult.response.find(
               "3\r\none\r\n0\r\nx-result: one\r\n\r\n") !=
           std::string::npos);
    assert(clientResult.response.find("0\r\nx-result: two\r\n\r\n") !=
           std::string::npos);
}

void nonBufferingChunkStateResetsAcrossKeepAlive() {
    us_listen_socket_t *listenSocket = nullptr;
    ClientResult clientResult;
    std::thread client;
    unsigned int requests = 0;

    {
        uWS::App app;
        app.any("/*", [&](auto *response, auto */*request*/) {
            response->onAborted([]() { assert(false && "pipeline aborted"); });
            const std::string_view payload = requests++ ? "two" : "one";
            const auto written = response->tryWriteChunk(payload);
            assert(written.valid && !written.blocked);
            assert(written.consumed == payload.size());
            response->end();
            if (requests == 2U) {
                us_listen_socket_close(0, listenSocket);
            }
        }).listen("127.0.0.1", 0, [&](auto *token) {
            assert(token);
            listenSocket = token;
            int port = us_socket_local_port(
                0, reinterpret_cast<us_socket_t *>(listenSocket));
            client = std::thread([&clientResult, port]() {
                clientResult = exchange(
                    port,
                    "GET /one HTTP/1.1\r\nHost: example.test\r\n\r\n"
                    "GET /two HTTP/1.1\r\nHost: example.test\r\n"
                    "Connection: close\r\n\r\n");
            });
        }).run();
    }

    client.join();
    assert(clientResult.error == 0);
    assert(requests == 2U);
    assert(clientResult.response.find("3\r\none\r\n0\r\n\r\n") !=
           std::string::npos);
    assert(clientResult.response.find("3\r\ntwo\r\n0\r\n\r\n") !=
           std::string::npos);
}

void chunkWriteModesCannotMix() {
    us_listen_socket_t *listenSocket = nullptr;
    ClientResult clientResult;
    std::thread client;
    unsigned int requests = 0;

    {
        uWS::App app;
        app.any("/*", [&](auto *response, auto */*request*/) {
            response->onAborted([]() { assert(false && "mixed mode aborted"); });
            if (requests++ == 0U) {
                const auto written = response->tryWriteChunk("modern");
                assert(written.valid && !written.blocked);
                assert(written.consumed == 6U);
                assert(!response->write("legacy"));
            } else {
                assert(response->write("legacy"));
                const auto rejected = response->tryWriteChunk("modern");
                assert(!rejected.valid && !rejected.blocked);
                assert(rejected.consumed == 0U);
                const auto trailerRejected =
                    response->endChunkedWithTrailers("x-result: no\r\n");
                assert(!trailerRejected.valid && !response->hasResponded());
            }
            response->end();
            if (requests == 2U) {
                us_listen_socket_close(0, listenSocket);
            }
        }).listen("127.0.0.1", 0, [&](auto *token) {
            assert(token);
            listenSocket = token;
            int port = us_socket_local_port(
                0, reinterpret_cast<us_socket_t *>(listenSocket));
            client = std::thread([&clientResult, port]() {
                clientResult = exchange(
                    port,
                    "GET /try HTTP/1.1\r\nHost: example.test\r\n\r\n"
                    "GET /legacy HTTP/1.1\r\nHost: example.test\r\n"
                    "Connection: close\r\n\r\n");
            });
        }).run();
    }

    client.join();
    assert(clientResult.error == 0);
    assert(requests == 2U);
    assert(clientResult.response.find("6\r\nmodern\r\n0\r\n\r\n") !=
           std::string::npos);
    assert(clientResult.response.find("6\r\nlegacy\r\n0\r\n\r\n") !=
           std::string::npos);
}

} // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);

    requestTrailersPrecedeBodyEnd();
    uWS::Loop::get()->free();
    nonTrailerRequestReleasesHandlerAtBodyEnd();
    uWS::Loop::get()->free();
    nonBufferingChunkWriteRetriesExactSuffix();
    uWS::Loop::get()->free();
    chunkedTrailerFinalCopiesBoundedInput();
    uWS::Loop::get()->free();
    chunkedTrailerFinalSupportsTrailerOnlyAndKeepAliveReset();
    uWS::Loop::get()->free();
    nonBufferingChunkStateResetsAcrossKeepAlive();
    uWS::Loop::get()->free();
    chunkWriteModesCannotMix();
    uWS::Loop::get()->free();
}
