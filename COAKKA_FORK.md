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

## Slice State

- H0c1: raw request-target forms and route-controlled automatic continue are
  implemented and covered by `tests/CoAkkaHttp1.cpp`.
- H0c2: bounded request-trailer parsing and trailer-before-body-end delivery is
  still required before CoAkka HTTP Runtime may pin this fork.

CoAkka HTTP Runtime must not update its dependency lock to an intermediate fork
commit that still emits body end before request trailers are parsed and
validated.
