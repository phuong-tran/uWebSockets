#include <algorithm>
#include <cassert>
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

ClientResult exchange(int port, std::string_view request) {
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

} // namespace

int main() {
    std::signal(SIGPIPE, SIG_IGN);

    requestTrailersPrecedeBodyEnd();
    uWS::Loop::get()->free();
    nonTrailerRequestReleasesHandlerAtBodyEnd();
    uWS::Loop::get()->free();
}
