# Protocol Behavior

This document describes the `scribe.v0` wire protocol and how the current
rewrite splits responsibility between addon, engine, and IO.

The protocol runs on top of a reliable byte stream, currently a Unix domain
socket. Each frame is sent as:

1. a 32-bit big-endian payload length
2. a protobuf-encoded `scribe.v0.Envelope`

The protobuf payload size must be between 1 and `64 * 1024` (`65536`) bytes,
inclusive. Receivers must reject the current connection if a frame declares a
payload outside that range.

`scribe.v0` is intentionally strict. Exact version match is required, and
unexpected messages terminate the current connection generation.

## Layers

- addon: adapts fcitx events into engine events and applies engine effects back
  into fcitx and IO
- engine: synchronous session state machine that bridges fcitx-side intent and
  an authenticated connection
- IO: asynchronous transport actor that owns socket lifecycle, framing,
  handshake, authentication, and post-authentication message dispatch

This split keeps the core session rules testable without fcitx or live sockets.

## Roles

- `fcitx5-scribe` is the client
- the transcriber process is the server

## Connection Lifecycle

The engine uses three connection states:

- `Disconnected`
- `Connecting`
- `Ready`

The expected progression is:

1. user toggles transcription in a focused context
2. engine records a pending context and emits `Connect`
3. addon passes `ConnectionConfig` into `Io::connect`
4. IO connects the Unix socket and runs the handshake
5. IO reports `ConnectionAuthenticated`
6. engine transitions to `Ready`
7. engine emits `StartSession` for the pending context

If a failure happens before authentication completes, IO reports
`ConnectionFailed` and the engine drops pending startup. If an authenticated
connection closes cleanly, IO reports `ConnectionClosed`.

## Handshake

The handshake is fully owned by IO. The engine never sees unauthenticated
traffic.

The client sends `ClientHello` with:

- `protocol_version`
- `client_name`
- random `client_nonce`

The server replies with `ServerHello` containing:

- `accepted`
- `protocol_version`
- `error_message` when `accepted` is false
- random `server_nonce` when `accepted` is true
- `proof` when `accepted` is true

The proof is:

`HMAC-SHA256(token, "fcitx5-scribe/v0/server" || client_nonce || server_nonce)`

Validation rules for an accepted handshake:

- `accepted` must be true
- `protocol_version` must match exactly
- `server_nonce` must be exactly 32 bytes
- `proof` must match the locally computed HMAC

Handshake failures terminate the connection attempt inside IO and surface as
`ConnectionFailed`.

## Authenticated Message Flow

After authentication succeeds, IO only forwards these envelopes to the engine:

- `CommitText`
- `VadTimeout`
- `Error`

Any other message on an authenticated connection is treated as a protocol
violation and fails the connection.

## Session Lifecycle

At most one session is active at a time.

Once the connection is `Ready`, the engine may emit `StartSession` for the
currently pending context. The engine only does this when the context is still:

- alive
- focused

The session id is generated locally and treated as opaque by the addon and IO.

## Interruptions

The engine interrupts a session when:

- the user toggles again
- focus is lost
- focus moves to another input context
- the input context is destroyed
- commit ordering is violated

When the connection is still `Ready`, the engine emits `InterruptSession`
before clearing local session state. If the connection is already gone,
interruption is local-only.

## CommitText Rules

`CommitText` is accepted only when:

- its `session_id` matches the active session
- its `sequence_no` matches the next expected sequence number
- the target input context is still alive and focused

Sequence numbers start at `1` and increase by one for each accepted commit.

Out-of-order commits are treated as a transport or protocol failure signal. The
engine interrupts the session instead of accepting potentially unsafe text.

## Session End Messages

The transcriber may end a session by sending:

- `VadTimeout`
- `Error`

These messages only apply to the active session id. Messages for inactive
sessions are ignored.

## Invalid Frames And Messages

IO terminates the current connection generation on:

- connect timeout or handshake timeout
- invalid frame sizes
- invalid protobuf payloads
- unexpected messages during handshake
- unexpected messages after authentication
- failed transcriber proof validation

This keeps the engine focused on session policy while IO owns transport and
authentication correctness.
