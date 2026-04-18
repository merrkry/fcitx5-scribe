#!/usr/bin/env python3

import argparse
import asyncio
import hmac
import logging
import os
import signal
import stat
import struct
from dataclasses import dataclass
from hashlib import sha256
from pathlib import Path
from typing import Optional

from google.protobuf.message import DecodeError

# It seems LSP doesn't play well with current generation method.
# Consider init a uv workspace for import to work properly.
from scribe.v0.scribe_pb2 import (
    ClientHello,
    CommitText,
    Envelope,
    Error,
    ErrorCode,
    InterruptReason,
    InterruptSession,
    ServerHello,
    StartSession,
    VadTimeout,
)

PROTOCOL_VERSION = "scribe.v0"
SERVER_PROOF_LABEL = b"fcitx5-scribe/v0/server"
MAX_FRAME_SIZE = 64 * 1024
logger = logging.getLogger(__name__)


def unlink_socket_if_present(path: Path) -> None:
    try:
        status = path.lstat()
    except FileNotFoundError:
        return
    if not stat.S_ISSOCK(status.st_mode):
        raise RuntimeError(f"{path} exists and is not a socket")
    path.unlink()


def positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be > 0")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--socket-path", default=os.environ.get("FCITX5_SCRIBE_DEBUG_SOCKET_PATH")
    )
    parser.add_argument(
        "--auth-token", default=os.environ.get("FCITX5_SCRIBE_DEBUG_AUTH_TOKEN")
    )
    parser.add_argument(
        "--mode",
        choices=("result", "vad-timeout", "error"),
        default="result",
    )
    parser.add_argument("--interval-seconds", type=positive_float, default=1.0)
    parser.add_argument(
        "--stop-after-seconds",
        type=positive_float,
        default=5.0,
        help="delay before vad-timeout or error mode emits its terminal event",
    )
    args = parser.parse_args()
    if not args.socket_path:
        parser.error(
            "--socket-path is required or set FCITX5_SCRIBE_DEBUG_SOCKET_PATH in the environment"
        )
    if not args.auth_token:
        parser.error(
            "--auth-token is required or set FCITX5_SCRIBE_DEBUG_AUTH_TOKEN in the environment"
        )
    return args


def compute_server_proof(token: str, client_nonce: bytes, server_nonce: bytes) -> bytes:
    return hmac.new(
        token.encode("utf-8"),
        SERVER_PROOF_LABEL + client_nonce + server_nonce,
        sha256,
    ).digest()


async def read_exact(reader: asyncio.StreamReader, size: int) -> bytes:
    return await reader.readexactly(size)


async def read_envelope(reader: asyncio.StreamReader) -> Envelope:
    raw_size = await read_exact(reader, 4)
    (frame_size,) = struct.unpack("!I", raw_size)
    if frame_size == 0 or frame_size > MAX_FRAME_SIZE:
        raise ValueError("invalid frame size")
    payload = await read_exact(reader, frame_size)
    envelope = Envelope()
    try:
        envelope.ParseFromString(payload)
    except DecodeError as exc:
        raise ValueError("invalid protobuf payload") from exc
    return envelope


async def write_envelope(writer: asyncio.StreamWriter, envelope: Envelope) -> None:
    payload = envelope.SerializeToString()
    if len(payload) == 0 or len(payload) > MAX_FRAME_SIZE:
        raise ValueError("invalid outbound frame size")
    writer.write(struct.pack("!I", len(payload)))
    writer.write(payload)
    await writer.drain()


@dataclass
class SessionState:
    session_id: bytes
    sequence_no: int = 1
    task: Optional[asyncio.Task] = None


class TranscriberServer:
    def __init__(
        self, token: str, mode: str, interval_seconds: float, stop_after_seconds: float
    ) -> None:
        self._token = token
        self._mode = mode
        self._interval_seconds = interval_seconds
        self._stop_after_seconds = stop_after_seconds

    async def handle_client(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        handler = TranscriberConnection(
            token=self._token,
            mode=self._mode,
            interval_seconds=self._interval_seconds,
            stop_after_seconds=self._stop_after_seconds,
        )
        await handler.handle(reader, writer)


class TranscriberConnection:
    def __init__(
        self, token: str, mode: str, interval_seconds: float, stop_after_seconds: float
    ) -> None:
        self._token = token
        self._mode = mode
        self._interval_seconds = interval_seconds
        self._stop_after_seconds = stop_after_seconds
        self._session: Optional[SessionState] = None

    async def handle(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        peer = writer.get_extra_info("peername")
        try:
            hello = await read_envelope(reader)
            if not hello.HasField("client_hello"):
                raise ValueError("expected ClientHello")

            client_hello: ClientHello = hello.client_hello
            if client_hello.protocol_version != PROTOCOL_VERSION:
                logger.info(
                    "client %r offered unsupported protocol version %r",
                    peer,
                    client_hello.protocol_version,
                )
                await self._reject(writer, "protocol version mismatch")
                return
            if len(client_hello.client_nonce) != 32:
                await self._reject(writer, "invalid client nonce")
                return

            server_nonce = os.urandom(32)
            server_hello = Envelope(
                server_hello=ServerHello(
                    protocol_version=PROTOCOL_VERSION,
                    accepted=True,
                    server_nonce=server_nonce,
                    proof=compute_server_proof(
                        self._token, client_hello.client_nonce, server_nonce
                    ),
                )
            )
            await write_envelope(writer, server_hello)

            while True:
                envelope = await read_envelope(reader)
                if envelope.HasField("start_session"):
                    await self._handle_start(envelope.start_session, writer)
                    continue
                if envelope.HasField("interrupt_session"):
                    await self._handle_interrupt(envelope.interrupt_session)
                    continue
                raise ValueError("unexpected envelope payload")
        except (asyncio.IncompleteReadError, BrokenPipeError, ConnectionResetError):
            pass
        except Exception as exc:
            logger.exception("client %r failed: %s", peer, exc)
        finally:
            await self._clear_session()
            writer.close()
            await writer.wait_closed()

    async def _reject(self, writer: asyncio.StreamWriter, reason: str) -> None:
        await write_envelope(
            writer,
            Envelope(
                server_hello=ServerHello(
                    protocol_version=PROTOCOL_VERSION,
                    accepted=False,
                    error_message=reason,
                )
            ),
        )

    async def _handle_start(
        self, start: StartSession, writer: asyncio.StreamWriter
    ) -> None:
        if len(start.session_id) != 16:
            raise ValueError("invalid session id size")
        if self._session is not None:
            raise ValueError("received StartSession while another session is active")
        session = SessionState(session_id=start.session_id)
        session.task = asyncio.create_task(self._run_session(session, writer))
        self._session = session

    async def _handle_interrupt(self, interrupt: InterruptSession) -> None:
        if len(interrupt.session_id) != 16:
            raise ValueError("invalid session id size")
        if self._session is None:
            return
        if interrupt.session_id != self._session.session_id:
            return
        if interrupt.reason == InterruptReason.INTERRUPT_REASON_UNSPECIFIED:
            raise ValueError("interrupt reason must be set")
        await self._clear_session()

    async def _run_session(
        self, session: SessionState, writer: asyncio.StreamWriter
    ) -> None:
        if self._mode == "result":
            elapsed = 0.0
            # Result mode is intentionally open-ended: v0 has no successful
            # terminal event, so stopping silently would leave the addon active.
            while True:
                await asyncio.sleep(self._interval_seconds)
                if (
                    self._session is None
                    or self._session.session_id != session.session_id
                ):
                    return
                elapsed += self._interval_seconds
                await write_envelope(
                    writer,
                    Envelope(
                        commit_text=CommitText(
                            session_id=session.session_id,
                            sequence_no=session.sequence_no,
                            text=f"{elapsed:g}",
                        )
                    ),
                )
                session.sequence_no += 1
        elif self._mode == "vad-timeout":
            await asyncio.sleep(self._stop_after_seconds)
            if self._session is None or self._session.session_id != session.session_id:
                return
            await write_envelope(
                writer, Envelope(vad_timeout=VadTimeout(session_id=session.session_id))
            )
            await self._clear_session()
        else:
            await asyncio.sleep(self._stop_after_seconds)
            if self._session is None or self._session.session_id != session.session_id:
                return
            await write_envelope(
                writer,
                Envelope(
                    error=Error(
                        session_id=session.session_id,
                        code=ErrorCode.ERROR_CODE_TRANSCRIBER_INTERNAL,
                        message="simulated error",
                    )
                ),
            )
            await self._clear_session()

    async def _clear_session(self) -> None:
        if self._session is None:
            return
        task = self._session.task
        self._session = None
        if task is not None and task is not asyncio.current_task():
            if not task.done():
                task.cancel()
            try:
                await task
            except (asyncio.CancelledError, BrokenPipeError, ConnectionResetError):
                pass
            except Exception:
                logger.debug("session task failed during cleanup", exc_info=True)


async def main() -> None:
    logging.basicConfig(level=logging.INFO)
    args = parse_args()
    socket_path = Path(args.socket_path)
    socket_path.parent.mkdir(parents=True, exist_ok=True)
    unlink_socket_if_present(socket_path)

    server = TranscriberServer(
        token=args.auth_token,
        mode=args.mode,
        interval_seconds=args.interval_seconds,
        stop_after_seconds=args.stop_after_seconds,
    )

    loop = asyncio.get_running_loop()
    stop_event = asyncio.Event()

    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, stop_event.set)

    server_handle = await asyncio.start_unix_server(
        server.handle_client, path=args.socket_path
    )
    logger.info("listening on %s", socket_path)
    try:
        await stop_event.wait()
    finally:
        server_handle.close()
        await server_handle.wait_closed()
        unlink_socket_if_present(socket_path)


if __name__ == "__main__":
    asyncio.run(main())
