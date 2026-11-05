"""Hashta: a terminal chat loop over ChatEngine.

Run with:  py main.py

The turn loop itself lives in ``engine.py``. This file is a renderer: it
turns engine events into the same lines the harness has always printed, and
keeps the terminal-only concerns (prompt reading, /commands).
"""

import sys
from typing import Any, Dict

import memory
from engine import ChatEngine

HELP_TEXT = """Commands:
  /reset   forget the current conversation (long-term memory is kept)
  /memory  show what was recalled for the last message
  /help    show this help
  /exit    quit (also /quit or Ctrl+C)"""

# Typed shortcuts and their actions.
COMMANDS = {
    "/exit": "quit",
    "/quit": "quit",
    "/reset": "reset",
    "/memory": "memory",
    "/help": "help",
}


def _configure_console() -> None:
    """Force UTF-8 on all three streams.

    The model replies in emoji and the user may type them. A real Windows
    console goes through WriteConsoleW and does not care, but a redirected
    stream or a ConPTY terminal (VS Code, Windows Terminal) is a pipe using
    the legacy ANSI code page: emoji become "?" on output and lone surrogates
    on input, and the latter would fail when written to SQLite.
    """
    for stream in (sys.stdin, sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


def _read_line(prompt: str):
    """Read one line, returning None on EOF (piped input) or Ctrl+C."""
    try:
        return input(prompt)
    except (EOFError, KeyboardInterrupt):
        print()
        return None


class Terminal:
    """Renders engine events as terminal text."""

    def __init__(self) -> None:
        self.streamed = False
        self.line_open = False

    def __call__(self, event: Dict[str, Any]) -> None:
        kind = event["type"]
        if kind == "status":
            self._status(event)
        elif kind == "delta":
            if not self.streamed:
                print("Hashta: ", end="", flush=True)
                self.streamed = True
            self.line_open = True
            print(event["text"], end="", flush=True)
        elif kind == "tool":
            self._tool(event)
        elif kind == "reply":
            self._close_line()
            self.streamed = False
        elif kind == "error":
            print(f"\n[error] {event['message']}")
            self.streamed = False
            self.line_open = False
        elif kind == "cancelled":
            print("\n[interrupted]")
            self.streamed = False
            self.line_open = False

    # -- internals ---------------------------------------------------------

    def _status(self, event: Dict[str, Any]) -> None:
        phase = event["phase"]
        if phase == "boot":
            print(event["text"])
        elif phase == "warning":
            print(f"\n[warning] {event['text']}")

    def _tool(self, event: Dict[str, Any]) -> None:
        if event["phase"] in ("call", "limit"):
            self._close_line()
            print(f"[memory] {event['text']}", flush=True)

    def _close_line(self) -> None:
        if self.line_open:
            print()
            self.line_open = False


def _show_memory(engine: ChatEngine) -> None:
    """Print what was recalled for the previous message."""
    hits = engine.last_hits
    if not hits:
        print("[memory] nothing was recalled for the last message")
        return
    block = engine.conversation.memory_block
    print(
        f"[memory] {len(hits)} hit(s), block "
        f"{memory.estimate_tokens(block)} tokens:"
    )
    for hit in memory.order_hits(hits):
        stamp = memory.format_local(hit.created_at)
        preview = " ".join(hit.content.split())[:70]
        print(f"  {hit.score:.3f}  {stamp}  {hit.role:<9} {preview!r}")


def main() -> int:
    _configure_console()
    print("Hashta - local companion chat")
    print("Type /help for commands.\n")

    engine = ChatEngine(emit=Terminal())
    try:
        engine.start()
    except (RuntimeError, SystemExit) as exc:
        print(f"\n[error] {exc}")
        engine.close()
        return 1

    try:
        while True:
            line = _read_line("\nYou: ")
            if line is None:
                break

            text = line.strip()
            if not text:
                continue

            command = COMMANDS.get(text.lower())
            if command == "quit":
                break
            if command == "reset":
                engine.reset()
                print("[conversation cleared - long-term memory kept]")
                continue
            if command == "memory":
                _show_memory(engine)
                continue
            if command == "help":
                print(HELP_TEXT)
                continue

            engine.send(text)
    except KeyboardInterrupt:
        pass
    finally:
        print()
        engine.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
