#include <cassert>
#include <algorithm>
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
using RequestTrailerHandler =
    uWS::MoveOnlyFunction<void(uWS::HttpRequestTrailers *)>;
using ParserRequestHandler =
    uWS::MoveOnlyFunction<void *(void *, uWS::HttpRequest *)>;
using ParserDataHandler = uWS::MoveOnlyFunction<
    void *(void *, std::string_view, uint64_t)>;
using LegacyParserConsume = std::pair<unsigned int, void *> (
    uWS::HttpParser::*)(char *, unsigned int, void *, void *,
                       ParserRequestHandler &&, ParserDataHandler &&);
struct UpstreamParserLayout {
    std::string fallback;
    uint64_t remainingStreamingBytes;
};

static_assert(std::is_same_v<
              decltype(std::declval<uWS::App &>().any(
                  std::declval<std::string>(),
                  std::declval<PlaintextRouteHandler>(),
                  uWS::HttpRouteOptions{false})),
              uWS::App &&>);
static_assert(std::is_same_v<
              decltype(&uWS::HttpParser::consumePostPadded),
              LegacyParserConsume>);
static_assert(sizeof(uWS::HttpParser) == sizeof(UpstreamParserLayout));
static_assert(std::is_constructible_v<uWS::App, uWS::SocketContextOptions,
                                      uWS::HttpContextOptions>);
static_assert(std::is_same_v<
              decltype(std::declval<uWS::HttpResponse<false> &>()
                           .onRequestTrailers(
                               std::declval<RequestTrailerHandler>())),
              void>);
static_assert(std::is_same_v<
              decltype(std::declval<uWS::HttpResponse<false> &>()
                           .tryWriteChunk(std::declval<std::string_view>())),
              uWS::HttpChunkWriteResult>);
static_assert(std::is_same_v<
              decltype(std::declval<uWS::HttpResponse<false> &>()
                           .endChunkedWithTrailers(
                               std::declval<std::string_view>())),
              uWS::HttpChunkTrailerEndResult>);
static_assert(
    std::is_same_v<decltype(uWS::HttpChunkTrailerEndResult::bufferedBytes),
                   unsigned int>);
static_assert(
    std::is_same_v<decltype(uWS::HttpChunkTrailerEndResult::valid), bool>);

struct Observation {
    unsigned int heads = 0;
    unsigned int finals = 0;
    unsigned int trailerCallbacks = 0;
    std::string method;
    std::string target;
    std::string body;
    std::vector<std::pair<std::string, std::string>> trailers;
    std::vector<std::string> events;
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
            observation.events.emplace_back("head");
            return inputUser;
        },
        [&observation](void *inputUser, std::string_view chunk,
                       uint64_t remaining) -> void * {
            if (remaining == 0) {
                observation.finals++;
                observation.events.emplace_back("end");
            } else {
                observation.body.append(chunk);
                observation.events.emplace_back("body");
            }
            return inputUser;
        });
    return {result.first, result.second == &user, result.second == uWS::FULLPTR,
            std::move(observation)};
}

ParseResult parseWithTrailers(std::string_view wire, std::size_t fragmentSize,
                              bool stopAtTrailers = false) {
    assert(fragmentSize > 0);
    uWS::HttpParser parser;
    int user = 0;
    ParseResult result;
    result.returnedUser = true;

    for (std::size_t offset = 0; offset < wire.size();) {
        std::size_t pieceSize =
            std::min(fragmentSize, wire.size() - offset);
        std::vector<char> bytes(pieceSize + uWS::MINIMUM_HTTP_POST_PADDING);
        std::memcpy(bytes.data(), wire.data() + offset, pieceSize);

        auto parsed = parser.consumePostPaddedWithTrailers(
            bytes.data(), static_cast<unsigned int>(pieceSize), &user, nullptr,
            [&result](void *inputUser, uWS::HttpRequest *request) -> void * {
                result.observation.heads++;
                result.observation.method = request->getCaseSensitiveMethod();
                result.observation.target = request->getFullUrl();
                result.observation.events.emplace_back("head");
                return inputUser;
            },
            [&result](void *inputUser, std::string_view chunk,
                      uint64_t remaining) -> void * {
                if (remaining == 0) {
                    result.observation.finals++;
                    result.observation.events.emplace_back("end");
                } else {
                    result.observation.body.append(chunk);
                    result.observation.events.emplace_back("body");
                }
                return inputUser;
            },
            [&result, stopAtTrailers](
                void *inputUser, uWS::HttpRequestTrailers *trailers) -> void * {
                result.observation.trailerCallbacks++;
                result.observation.events.emplace_back("trailers");
                for (auto [name, value] : *trailers) {
                    result.observation.trailers.emplace_back(name, value);
                }
                return stopAtTrailers ? nullptr : inputUser;
            });

        result.consumedOrError = parsed.first;
        if (parsed.second == uWS::FULLPTR) {
            result.parserError = true;
            result.returnedUser = false;
            break;
        }
        if (parsed.second != &user) {
            result.returnedUser = false;
            break;
        }
        offset += pieceSize;
    }
    return result;
}

} // namespace

int main() {
    uWS::HttpRouteOptions defaultOptions;
    assert(defaultOptions.automaticContinue);
    uWS::HttpRouteOptions runtimeOwnedContinue{false};
    assert(!runtimeOwnedContinue.automaticContinue);
    uWS::HttpContextOptions defaultContextOptions;
    assert(!defaultContextOptions.requestTrailers);
    uWS::HttpContextOptions trailerAwareContext{true};
    assert(trailerAwareContext.requestTrailers);

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

    const std::string trailerWire =
        "POST /trailers HTTP/1.1\r\nHost: example.test\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "3\r\nabc\r\n0\r\nX-Checksum: value\r\nX-Extra:\tsecond \t\r\n\r\n";
    for (std::size_t fragmentSize = 1; fragmentSize <= trailerWire.size();
         fragmentSize++) {
        auto trailerResult = parseWithTrailers(trailerWire, fragmentSize);
        assert(trailerResult.returnedUser);
        assert(!trailerResult.parserError);
        assert(trailerResult.observation.heads == 1);
        assert(trailerResult.observation.body == "abc");
        assert(trailerResult.observation.trailerCallbacks == 1);
        assert(trailerResult.observation.finals == 1);
        assert(trailerResult.observation.trailers.size() == 2);
        assert(trailerResult.observation.trailers[0] ==
               std::make_pair(std::string("x-checksum"),
                              std::string("value")));
        assert(trailerResult.observation.trailers[1] ==
               std::make_pair(std::string("x-extra"),
                              std::string("second")));
        auto trailersEvent =
            std::find(trailerResult.observation.events.begin(),
                      trailerResult.observation.events.end(), "trailers");
        auto endEvent = std::find(trailerResult.observation.events.begin(),
                                  trailerResult.observation.events.end(),
                                  "end");
        assert(trailersEvent < endEvent);
    }

    auto emptyTrailers = parseWithTrailers(
        "POST /trailers HTTP/1.1\r\nHost: example.test\r\n"
        "Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
        1);
    assert(emptyTrailers.returnedUser);
    assert(!emptyTrailers.parserError);
    assert(emptyTrailers.observation.trailerCallbacks == 1);
    assert(emptyTrailers.observation.trailers.empty());
    assert(emptyTrailers.observation.finals == 1);

    auto stoppedAtTrailers =
        parseWithTrailers(trailerWire, trailerWire.size(), true);
    assert(!stoppedAtTrailers.returnedUser);
    assert(!stoppedAtTrailers.parserError);
    assert(stoppedAtTrailers.observation.trailerCallbacks == 1);
    assert(stoppedAtTrailers.observation.finals == 0);

    auto malformedTrailer = parseWithTrailers(
        "POST /trailers HTTP/1.1\r\nHost: example.test\r\n"
        "Transfer-Encoding: chunked\r\n\r\n0\r\nBad Name: value\r\n\r\n",
        3);
    assert(malformedTrailer.parserError);
    assert(malformedTrailer.consumedOrError ==
           uWS::HTTP_ERROR_400_BAD_REQUEST);
    assert(malformedTrailer.observation.trailerCallbacks == 0);
    assert(malformedTrailer.observation.finals == 0);

    auto emptyNameTrailer = parseWithTrailers(
        "POST /trailers HTTP/1.1\r\nHost: example.test\r\n"
        "Transfer-Encoding: chunked\r\n\r\n0\r\n: value\r\n\r\n",
        11);
    assert(emptyNameTrailer.parserError);
    assert(emptyNameTrailer.consumedOrError ==
           uWS::HTTP_ERROR_400_BAD_REQUEST);
    assert(emptyNameTrailer.observation.trailerCallbacks == 0);
    assert(emptyNameTrailer.observation.finals == 0);

    auto invalidLineEnding = parseWithTrailers(
        "POST /trailers HTTP/1.1\r\nHost: example.test\r\n"
        "Transfer-Encoding: chunked\r\n\r\n0\r\nX-Field: value\rX",
        4096);
    assert(invalidLineEnding.parserError);
    assert(invalidLineEnding.consumedOrError ==
           uWS::HTTP_ERROR_400_BAD_REQUEST);
    assert(invalidLineEnding.observation.trailerCallbacks == 0);
    assert(invalidLineEnding.observation.finals == 0);

    std::string oversizedTrailer =
        "POST /trailers HTTP/1.1\r\nHost: example.test\r\n"
        "Transfer-Encoding: chunked\r\n\r\n0\r\nX-Large: ";
    oversizedTrailer.append(uWS::MAX_TRAILER_FALLBACK_SIZE, 'a');
    oversizedTrailer.append("\r\n\r\n");
    auto oversizedResult = parseWithTrailers(oversizedTrailer, 17);
    assert(oversizedResult.parserError);
    assert(oversizedResult.consumedOrError ==
           uWS::HTTP_ERROR_431_REQUEST_HEADER_FIELDS_TOO_LARGE);
    assert(oversizedResult.observation.trailerCallbacks == 0);
    assert(oversizedResult.observation.finals == 0);

    std::string tooManyTrailers =
        "POST /trailers HTTP/1.1\r\nHost: example.test\r\n"
        "Transfer-Encoding: chunked\r\n\r\n0\r\n";
    for (unsigned int index = 0; index < UWS_HTTP_MAX_HEADERS_COUNT; index++) {
        tooManyTrailers.append("X-Field: value\r\n");
    }
    tooManyTrailers.append("\r\n");
    auto tooManyResult = parseWithTrailers(tooManyTrailers, 29);
    assert(tooManyResult.parserError);
    assert(tooManyResult.consumedOrError ==
           uWS::HTTP_ERROR_431_REQUEST_HEADER_FIELDS_TOO_LARGE);
    assert(tooManyResult.observation.trailerCallbacks == 0);
    assert(tooManyResult.observation.finals == 0);

    std::string maximumTrailerFields =
        "POST /trailers HTTP/1.1\r\nHost: example.test\r\n"
        "Transfer-Encoding: chunked\r\n\r\n0\r\n";
    for (unsigned int index = 0; index < UWS_HTTP_MAX_HEADERS_COUNT - 1;
         index++) {
        maximumTrailerFields.append("X-Field: value\r\n");
    }
    maximumTrailerFields.append("\r\n");
    auto maximumFieldsResult = parseWithTrailers(maximumTrailerFields, 29);
    assert(maximumFieldsResult.returnedUser);
    assert(!maximumFieldsResult.parserError);
    assert(maximumFieldsResult.observation.trailerCallbacks == 1);
    assert(maximumFieldsResult.observation.trailers.size() ==
           UWS_HTTP_MAX_HEADERS_COUNT - 1);
    assert(maximumFieldsResult.observation.finals == 1);

    std::string invalidValueTrailer =
        "POST /trailers HTTP/1.1\r\nHost: example.test\r\n"
        "Transfer-Encoding: chunked\r\n\r\n0\r\nX-Value: bad";
    invalidValueTrailer.push_back('\x01');
    invalidValueTrailer.append("value\r\n\r\n");
    auto invalidValueResult = parseWithTrailers(invalidValueTrailer, 7);
    assert(invalidValueResult.parserError);
    assert(invalidValueResult.consumedOrError ==
           uWS::HTTP_ERROR_400_BAD_REQUEST);
    assert(invalidValueResult.observation.trailerCallbacks == 0);
    assert(invalidValueResult.observation.finals == 0);

    const std::string pipelinedWire =
        trailerWire + "GET /next HTTP/1.1\r\nHost: example.test\r\n\r\n";
    for (std::size_t fragmentSize = 1; fragmentSize <= pipelinedWire.size();
         fragmentSize++) {
        auto pipelined = parseWithTrailers(pipelinedWire, fragmentSize);
        assert(pipelined.returnedUser);
        assert(!pipelined.parserError);
        assert(pipelined.observation.heads == 2);
        assert(pipelined.observation.trailerCallbacks == 1);
        assert(pipelined.observation.finals == 2);
    }

    std::string largePipeline = trailerWire;
    constexpr unsigned int pipelinedRequests = 128;
    for (unsigned int index = 0; index < pipelinedRequests; index++) {
        largePipeline.append(
            "GET /next HTTP/1.1\r\nHost: example.test\r\n\r\n");
    }
    auto largePipelined =
        parseWithTrailers(largePipeline, largePipeline.size());
    assert(largePipelined.returnedUser);
    assert(!largePipelined.parserError);
    assert(largePipelined.observation.heads == pipelinedRequests + 1);
    assert(largePipelined.observation.trailerCallbacks == 1);
    assert(largePipelined.observation.finals == pipelinedRequests + 1);

    auto legacyTrailer = parse(
        "POST /trailers HTTP/1.1\r\nHost: example.test\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        "3\r\nabc\r\n0\r\nX-Checksum: value\r\n\r\n");
    assert(legacyTrailer.parserError);
    assert(legacyTrailer.observation.trailerCallbacks == 0);
    assert(legacyTrailer.observation.finals == 1);
}
