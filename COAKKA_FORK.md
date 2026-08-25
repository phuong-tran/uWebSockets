# CoAkka uWebSockets Fork Contract

## Base And Purpose

This maintained fork starts from upstream uWebSockets commit
`fe7c01a477b688a7743f754fee33bdd78d52ad91` (`20.79.0`). It exists only to
expose HTTP/1 protocol events that CoAkka HTTP Runtime must own but the pinned
upstream API does not currently deliver.

The fork remains an HTTP transport provider. CoAkka HTTP Runtime owns request-
target semantics, expectation decisions, trailer policy, admission, resource
bounds, cancellation, and wire failure projection.

## Compatibility Law

- Existing `TemplatedApp::any(pattern, handler)` callers retain automatic
  `100 Continue` behavior.
- `HttpRouteOptions{false}` lets a route disable that middleware so the runtime
  can validate a request head before choosing `100` or a final response.
- The parser exposes the raw non-empty request-target token for origin-,
  absolute-, asterisk-, and authority-form requests. It does not assign method-
  specific semantics to those forms.
- Missing/duplicate Host and ambiguous Content-Length plus Transfer-Encoding
  remain provider-level errors before the route handler.
- Provider views remain callback-borrowed. This fork does not introduce an
  application callback ABI, allocator, thread, queue, socket owner, or retry.
- The explicit trailer-aware parser path stores at most 4 KiB of an incomplete
  trailer block and exposes at most `UWS_HTTP_MAX_HEADERS_COUNT - 1` validated
  fields (99 by default). Names are non-empty and lowercased, values trim
  surrounding SP/HTAB, and malformed or exhausted input fails before the body-
  end callback.
- Incomplete heads and trailers reuse the parser's existing fallback owner;
  their states are mutually exclusive. Trailer progress uses an otherwise
  unreachable value in the existing chunk state word, so the extended path
  adds no per-connection field, string, or steady-state allocation.
- Context opt-in adds one bounded move-only trailer-handler slot to each HTTP
  socket. It owns only the native adapter callback; trailer names and values
  remain borrowed parser views and are never retained by the provider.
- `HttpResponse::tryWriteChunk` is additive and cannot be mixed with legacy
  `write`. It emits canonical chunk framing, reports the exact payload prefix
  consumed, and never copies an unconsumed payload suffix into provider
  backpressure storage. A blocked caller retries only the exact remaining view
  after `onWritable`; retry before that event performs no I/O, and mismatched
  retry length and premature finalization fail before I/O. The provider may
  retain only fixed head/chunk/final framing bytes.
- One `size_t` field and one boolean in `HttpResponseData` record the
  caller-owned payload bytes remaining and the writable-event gate for the
  current chunk. Request reset and terminal completion clear them. Both are
  bounded by the existing live-connection ceiling and add no allocator, queue,
  thread, descriptor, timer, or callback owner.

## Slice State

- H0c1: raw request-target forms and route-controlled automatic continue are
  implemented and covered by `tests/CoAkkaHttp1.cpp`.
- H0c2a: the parser's explicit `consumePostPaddedWithTrailers` path validates
  bounded request trailers and delivers callback-extent fields before body end.
  The original six-argument `consumePostPadded` signature and behavior remain
  unchanged.
- H0c2b: `HttpContextOptions::requestTrailers` keeps the legacy parser path as
  the default. An opted-in context exposes callback-extent fields through
  `HttpResponse::onRequestTrailers` before the final `onDataV2` event. The
  per-request handler moves out at trailer delivery or clears at body end, so a
  keep-alive socket cannot retain it into the next idle period.
- H3a6b0: `tryWriteChunk` supplies the exact non-buffering provider primitive
  required by CoAkka's lane-owned partial offsets. The context test forces a
  4 MiB loopback write through partial `onWritable` retries, bounds provider
  buffering to framing, verifies exact chunk bytes/finalization, rejects a
  mismatched suffix, and proves keep-alive state reset.

CoAkka HTTP Runtime must not update its dependency lock to an intermediate fork
commit that still emits body end before request trailers are parsed and
validated or retains an unreported response payload suffix.
