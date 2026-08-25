#include <cassert>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "../src/App.h"

namespace {

using PlaintextRouteHandler = uWS::MoveOnlyFunction<
    void(uWS::HttpResponse<false> *, uWS::HttpRequest *)>;

static_assert(std::is_same_v<
              decltype(std::declval<uWS::App &>().any(
                  std::declval<std::string>(),
                  std::declval<PlaintextRouteHandler>(),
                  uWS::HttpRouteOptions{false})),
              uWS::App &&>);

struct Observation {
    unsigned int heads = 0;
    std::string method;
    std::string target;
};

struct ParseResult {
    unsigned int consumedOrError = 0;
    bool returnedUser = false;
    bool parserError = false;
    Observation observation;
};

ParseResult parse(std::string_view wire) {
    std::vector<char> bytes(wire.size() + uWS::MINIMUM_HTTP_POST_PADDING);
    std::memcpy(bytes.data(), wire.data(), wire.size());
    uWS::HttpParser parser;
    int user = 0;
    Observation observation;
    auto result = parser.consumePostPadded(
        bytes.data(), static_cast<unsigned int>(wire.size()), &user, nullptr,
        [&observation](void *inputUser, uWS::HttpRequest *request) -> void * {
            observation.heads++;
            observation.method = request->getCaseSensitiveMethod();
            observation.target = request->getFullUrl();
            return inputUser;
        },
        [](void *inputUser, std::string_view, uint64_t) -> void * {
            return inputUser;
        });
    return {result.first, result.second == &user, result.second == uWS::FULLPTR,
            std::move(observation)};
}

} // namespace

int main() {
    uWS::HttpRouteOptions defaultOptions;
    assert(defaultOptions.automaticContinue);
    uWS::HttpRouteOptions runtimeOwnedContinue{false};
    assert(!runtimeOwnedContinue.automaticContinue);

    auto origin = parse("GET /resource?q=1 HTTP/1.1\r\nHost: example.test\r\n\r\n");
    assert(origin.returnedUser);
    assert(!origin.parserError);
    assert(origin.observation.heads == 1);
    assert(origin.observation.method == "GET");
    assert(origin.observation.target == "/resource?q=1");

    auto absolute = parse(
        "GET http://example.test/resource?q=1 HTTP/1.1\r\n"
        "Host: conflicting.test\r\n\r\n");
    assert(absolute.returnedUser);
    assert(!absolute.parserError);
    assert(absolute.observation.heads == 1);
    assert(absolute.observation.target ==
           "http://example.test/resource?q=1");

    auto asterisk = parse("OPTIONS * HTTP/1.1\r\nHost: example.test\r\n\r\n");
    assert(asterisk.returnedUser);
    assert(!asterisk.parserError);
    assert(asterisk.observation.heads == 1);
    assert(asterisk.observation.method == "OPTIONS");
    assert(asterisk.observation.target == "*");

    auto authority = parse(
        "CONNECT example.test:443 HTTP/1.1\r\nHost: example.test:443\r\n\r\n");
    assert(authority.returnedUser);
    assert(!authority.parserError);
    assert(authority.observation.heads == 1);
    assert(authority.observation.method == "CONNECT");
    assert(authority.observation.target == "example.test:443");

    auto emptyTarget = parse("GET  HTTP/1.1\r\nHost: example.test\r\n\r\n");
    assert(emptyTarget.parserError);
    assert(emptyTarget.consumedOrError ==
           uWS::HTTP_ERROR_505_HTTP_VERSION_NOT_SUPPORTED);
    assert(emptyTarget.observation.heads == 0);

    auto missingHost = parse("GET / HTTP/1.1\r\n\r\n");
    assert(missingHost.parserError);
    assert(missingHost.consumedOrError == uWS::HTTP_ERROR_400_BAD_REQUEST);
    assert(missingHost.observation.heads == 0);

    auto duplicateHost =
        parse("GET / HTTP/1.1\r\nHost: one\r\nHost: two\r\n\r\n");
    assert(duplicateHost.parserError);
    assert(duplicateHost.consumedOrError == uWS::HTTP_ERROR_400_BAD_REQUEST);
    assert(duplicateHost.observation.heads == 0);

    auto ambiguousFraming = parse(
        "POST / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 1\r\n"
        "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
    assert(ambiguousFraming.parserError);
    assert(ambiguousFraming.consumedOrError ==
           uWS::HTTP_ERROR_400_BAD_REQUEST);
    assert(ambiguousFraming.observation.heads == 0);
}
