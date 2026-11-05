"""The HTTP + SSE surface the panel talks to.

Nothing here knows what a turn is beyond "start one and watch the events".
The engine does the work; this module only moves bytes and enforces the rule
the CLI already relied on: one turn at a time.

Routes:

    GET  /events        SSE stream; opens with a `state` snapshot
    GET  /api/state     snapshot: phase, busy, live messages, counts
    GET  /api/health    200 when the service is up
    POST /api/send      {"text": "..."} -> 200, or 409 when busy
    POST /api/stop      cancel the running turn
    POST /api/reset     clear the live window -> 200, or 409 when busy
    POST /api/shutdown  answer, then stop the engine and the server
"""

import json
import queue
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, List, Optional

import config


class EventBus:
    """Fan-out of engine events to every connected client."""

    def __init__(self) -> None:
        self._subscribers: List["queue.Queue[Optional[Dict[str, Any]]]"] = []
        self._lock = threading.Lock()

    def subscribe(self) -> "queue.Queue[Optional[Dict[str, Any]]]":
        channel: "queue.Queue[Optional[Dict[str, Any]]]" = queue.Queue(maxsize=1024)
        with self._lock:
            self._subscribers.append(channel)
        return channel

    def unsubscribe(self, channel) -> None:
        with self._lock:
            if channel in self._subscribers:
                self._subscribers.remove(channel)

    def publish(self, event: Dict[str, Any]) -> None:
        with self._lock:
            subscribers = list(self._subscribers)
        for channel in subscribers:
            try:
                channel.put_nowait(event)
            except queue.Full:
                pass  # a stuck client must not stall the engine

    def close(self) -> None:
        """Unblock every stream so the server can exit."""
        with self._lock:
            subscribers = list(self._subscribers)
        for channel in subscribers:
            try:
                channel.put_nowait(None)
            except queue.Full:
                pass


class EngineService:
    """Serialises turns onto one worker thread."""

    def __init__(self, engine: Any, bus: EventBus) -> None:
        self.engine = engine
        self.bus = bus
        self._queue: "queue.Queue[Optional[str]]" = queue.Queue()
        self._lock = threading.Lock()
        self._busy = False
        self._thread = threading.Thread(target=self._run, name="turns", daemon=True)

    # -- lifecycle ---------------------------------------------------------

    def start(self) -> None:
        self._thread.start()

    def shutdown(self) -> None:
        self.stop()
        self._queue.put(None)
        self._thread.join(timeout=10)

    # -- commands ----------------------------------------------------------

    def submit(self, text: str) -> bool:
        if self.engine.phase != "ready":
            return False
        with self._lock:
            if self._busy:
                return False
            self._busy = True
        self._queue.put(text)
        return True

    def stop(self) -> None:
        self.engine.stop()

    def reset(self) -> bool:
        with self._lock:
            if self._busy:
                return False
        self.engine.reset()
        return True

    def snapshot(self) -> Dict[str, Any]:
        with self._lock:
            busy = self._busy
        engine = self.engine
        store = getattr(engine, "store", None)
        return {
            "type": "state",
            "phase": getattr(engine, "phase", "booting"),
            "busy": busy,
            "models": dict(getattr(engine, "models", {})),
            "messages": [
                {"role": turn["role"], "text": turn["content"]}
                for turn in list(engine.conversation.turns)
            ],
            "stats": {
                "messages": store.count() if store is not None else 0,
                "embedded": store.embedded_count() if store is not None else 0,
            },
        }

    # -- worker ------------------------------------------------------------

    def _run(self) -> None:
        while True:
            text = self._queue.get()
            if text is None:
                return
            try:
                self.engine.send(text)
            except Exception as exc:  # a bad turn must not kill the service
                self.bus.publish({"type": "error", "message": str(exc)})
                self.bus.publish(
                    {
                        "type": "reply",
                        "id": None,
                        "at": "",
                        "text": "",
                        "stopped": True,
                    }
                )
            finally:
                with self._lock:
                    self._busy = False


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "HashtaUI/1.0"

    def log_message(self, fmt: str, *args: Any) -> None:
        pass  # the engine's events are the log

    # -- routing -----------------------------------------------------------

    def do_GET(self) -> None:
        if self.path == "/events":
            self._events()
        elif self.path == "/api/state":
            self._json(200, self.server.service.snapshot())
        elif self.path == "/api/health":
            self._json(200, {"ok": True})
        else:
            self._json(404, {"ok": False, "error": "not found"})

    def do_POST(self) -> None:
        body = self._body()
        if self.path == "/api/send":
            text = str((body or {}).get("text") or "").strip()
            if not text:
                self._json(400, {"ok": False, "error": "text is required"})
                return
            accepted = self.server.service.submit(text)
            self._json(200 if accepted else 409, {"ok": accepted})
        elif self.path == "/api/stop":
            self.server.service.stop()
            self._json(200, {"ok": True})
        elif self.path == "/api/reset":
            accepted = self.server.service.reset()
            self._json(200 if accepted else 409, {"ok": accepted})
        elif self.path == "/api/shutdown":
            self._json(200, {"ok": True})
            self.server.shutdown_event.set()
        else:
            self._json(404, {"ok": False, "error": "not found"})

    # -- event stream ------------------------------------------------------

    def _events(self) -> None:
        # Chunked, not close-delimited: a plain body without Content-Length is
        # read by clients in fixed-size blocks and stalls the stream until the
        # connection ends. Chunk framing delivers each event as it is written.
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Transfer-Encoding", "chunked")
        self.send_header("Connection", "close")
        self.end_headers()

        channel = self.server.bus.subscribe()
        try:
            self._send_event(self.server.service.snapshot())
            while True:
                try:
                    event = channel.get(timeout=15)
                except queue.Empty:
                    self._write_chunk(b": ping\n\n")
                    continue
                if event is None:
                    break
                self._send_event(event)
            self._write_chunk(b"")
        except (BrokenPipeError, ConnectionResetError, OSError):
            pass
        finally:
            self.server.bus.unsubscribe(channel)
            self.close_connection = True

    def _send_event(self, event: Dict[str, Any]) -> None:
        payload = json.dumps(event, ensure_ascii=False).encode("utf-8")
        self._write_chunk(b"data: " + payload + b"\n\n")

    def _write_chunk(self, data: bytes) -> None:
        self.wfile.write(b"%X\r\n" % len(data) + data + b"\r\n")
        self.wfile.flush()

    # -- helpers -----------------------------------------------------------

    def _body(self) -> Dict[str, Any]:
        try:
            length = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            length = 0
        if length <= 0:
            return {}
        try:
            parsed = json.loads(self.rfile.read(length) or b"{}")
        except (json.JSONDecodeError, UnicodeDecodeError):
            return {}
        return parsed if isinstance(parsed, dict) else {}

    def _json(self, status: int, payload: Dict[str, Any]) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class UiServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    # Attributes the handler reads. Set by `serve`.
    service: EngineService
    bus: EventBus
    shutdown_event: threading.Event

    def handle_error(self, request: Any, client_address: Any) -> None:
        """Ignore clients that hang up between requests.

        A local HTTP client may close a keep-alive socket at any moment; the
        handler then fails reading the next request line and socketserver
        prints a full traceback. That is normal here and would bury real
        errors in the panel's log.
        """
        if isinstance(sys.exc_info()[1],
                      (ConnectionResetError, ConnectionAbortedError,
                       BrokenPipeError)):
            return
        super().handle_error(request, client_address)


def serve(
    service: EngineService,
    bus: EventBus,
    host: str = config.UI_HOST,
    port: int = config.UI_PORT,
    shutdown_event: Optional[threading.Event] = None,
) -> UiServer:
    """Bind the service. The caller decides which thread runs serve_forever."""
    httpd = UiServer((host, port), Handler)
    httpd.service = service
    httpd.bus = bus
    httpd.shutdown_event = shutdown_event or threading.Event()
    return httpd
