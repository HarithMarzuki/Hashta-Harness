"""The turn loop, with no output of its own.

Everything the terminal prints today and everything the panel will draw
tomorrow is derived from the events emitted here. Nothing in this module
prints: the CLI renders events as text, the UI will render them as bubbles.

Events (all dicts, all ``type`` first):

    status    phase boot|warning|ready, text
    user      id, at, text
    recall    tokens, hits[{id, role, at, score, preview}]
    tool      phase call|result|limit, text (call), result (result)
    delta     text
    reply     id, at, text, stopped
    error     message
    cancelled -
"""

import json
import uuid
from typing import Any, Callable, Dict, List, Optional

import config
import memory
from embedder import Embedder, backfill, embedding_server
from history import Conversation
from llama_server import chat_server
from llm_client import LlmError, stream_turn
from tools import TOOLS, Toolbox

Event = Dict[str, Any]
Emit = Callable[[Event], None]


class ChatEngine:
    """Owns the servers, the store, the conversation and the tool loop."""

    def __init__(self, emit: Optional[Emit] = None, store: Any = None) -> None:
        self.emit: Emit = emit or (lambda event: None)
        self.store = store
        self.embedder: Optional[Embedder] = None
        self.conversation = Conversation()
        self.session_id = uuid.uuid4().hex
        self.last_hits: List[memory.Hit] = []
        self._chat: Any = None
        self._embed_server: Any = None
        self._stop = False
        self._ready = False
        self.phase = "booting"
        # What the panel shows in its two status lines:
        # waiting -> loading -> ready, or failed / disabled.
        self.models = {"chat": "waiting", "embed": "waiting"}

    # -- lifecycle ---------------------------------------------------------

    def start(self) -> None:
        """Load the models and open the store.

        Raises only when the chat model cannot start: memory is an
        enhancement, so a broken embedder disables it and the chat goes on.
        """
        self.models["chat"] = "loading"
        self._chat = chat_server(log=lambda text: self._status("boot", text))
        try:
            self._chat.ensure_running()
        except (RuntimeError, SystemExit):
            self.models["chat"] = "failed"
            raise
        self.models["chat"] = "ready"

        self.models["embed"] = "loading"
        self._embed_server = embedding_server(
            log=lambda text: self._status("boot", text)
        )
        try:
            self._embed_server.ensure_running()
            self.embedder = Embedder()
            self.models["embed"] = "ready"
        except (RuntimeError, SystemExit) as exc:
            self._embed_server = None
            self.models["embed"] = "disabled"
            self._status("warning", f"long-term memory disabled: {exc}")

        if self.store is None:
            self.store = memory.open_store()
        if self.embedder is not None:
            filled = backfill(self.store, self.embedder)
            note = f", {filled} backfilled" if filled else ""
            self._status(
                "boot",
                f"[memory] {self.store.count()} messages, "
                f"{self.store.embedded_count()} embedded{note}",
            )
        self._ready = True
        self._status("ready")

    def close(self) -> None:
        if self.store is not None:
            self.store.close()
            self.store = None
        if self._embed_server is not None:
            self._embed_server.stop()
            self._embed_server = None
        if self._chat is not None:
            self._chat.stop()
            self._chat = None
        self._ready = False

    def stop(self) -> None:
        """Ask the turn in progress to end at the next event."""
        self._stop = True

    def reset(self) -> None:
        """Forget the live window; long-term memory is untouched."""
        self.conversation.reset()
        self.last_hits = []

    # -- one turn ----------------------------------------------------------

    def send(self, text: str) -> str:
        """Record the message, recall, run the tool loop, store the reply."""
        if not self._ready:
            raise RuntimeError("engine.start() has not completed")

        self._stop = False
        previous = [
            turn["content"]
            for turn in self.conversation.turns
            if turn["role"] == "assistant"
        ]
        query_text = _query_text(text, previous[-1] if previous else None)

        user_id = self._remember("user", text)
        self.conversation.add_user(text, user_id)
        self.emit(
            {
                "type": "user",
                "id": user_id,
                "at": memory.now_local(),
                "text": text,
            }
        )

        self.last_hits = self._recall(query_text)
        self.emit(
            {
                "type": "recall",
                "tokens": memory.estimate_tokens(self.conversation.memory_block),
                "hits": [_hit(hit) for hit in self.last_hits],
            }
        )

        seen = self.conversation.tracked_ids() | {hit.id for hit in self.last_hits}
        reply = self._answer(Toolbox(self.store, self.embedder, seen))

        if reply:
            assistant_id = self._remember("assistant", reply)
            self.conversation.add_assistant(reply, assistant_id)
            self.emit(
                {
                    "type": "reply",
                    "id": assistant_id,
                    "at": memory.now_local(),
                    "text": reply,
                    "stopped": False,
                }
            )
        else:
            # A cancelled or failed turn still ends the turn for the caller.
            self.emit(
                {
                    "type": "reply",
                    "id": None,
                    "at": memory.now_local(),
                    "text": "",
                    "stopped": True,
                }
            )
        return reply

    # -- the tool loop -----------------------------------------------------

    def _answer(self, toolbox: Toolbox) -> str:
        messages = list(self.conversation.messages())
        try:
            for _ in range(config.MEMORY_MAX_ROUNDS):
                text, calls = self._stream_round(messages, TOOLS)
                if self._stop:
                    self.emit({"type": "cancelled"})
                    return ""
                if not calls:
                    return text.strip()

                messages.append(_assistant_message(text, calls))
                for call in calls:
                    self.emit(
                        {
                            "type": "tool",
                            "phase": "call",
                            "text": _describe(call),
                        }
                    )
                    result = toolbox.call(call["name"], call["arguments"])
                    messages.append(
                        {
                            "role": "tool",
                            "tool_call_id": call["id"],
                            "content": result,
                        }
                    )
                    self.emit(
                        {
                            "type": "tool",
                            "phase": "result",
                            "text": _describe(call),
                            "result": result,
                        }
                    )

            # Round cap reached: one last pass with no tools available, so the
            # model has to answer with what it already has.
            self.emit(
                {
                    "type": "tool",
                    "phase": "limit",
                    "text": "round limit reached, answering",
                }
            )
            text, _ = self._stream_round(messages, None)
            return text.strip()
        except LlmError as exc:
            self.emit({"type": "error", "message": str(exc)})
            return ""
        except KeyboardInterrupt:
            # A half-finished answer is dropped from history on purpose: it is
            # not something the model actually said.
            self.emit({"type": "cancelled"})
            return ""

    def _stream_round(self, messages: List[dict], tools: Optional[List[dict]]):
        """Stream one completion, emitting deltas as they arrive."""
        pieces: List[str] = []
        calls: List[dict] = []
        stream = stream_turn(messages, tools=tools)
        try:
            for kind, payload in stream:
                if self._stop:
                    break
                if kind == "text":
                    pieces.append(payload)
                    self.emit({"type": "delta", "text": payload})
                else:
                    calls = payload
        finally:
            stream.close()
        return "".join(pieces), calls

    # -- memory ------------------------------------------------------------

    def _remember(self, role: str, text: str) -> int:
        message_id = self.store.record(self.session_id, role, text)
        if self.embedder is None:
            return message_id
        vector = self.embedder.embed(text, "doc")
        if vector is not None:
            self.store.set_embedding(message_id, vector, self.embedder.model_name)
        return message_id

    def _recall(self, query_text: str) -> List[memory.Hit]:
        """Retrieve memories for this turn and put them in the system prompt."""
        if self.embedder is None:
            self.conversation.set_memory_block("")
            return []

        vector = self.embedder.embed(query_text, "query")
        if vector is None:
            self.conversation.set_memory_block("")
            return []

        # No relative cut-off: the lines just above the floor are the
        # signposts the model follows with search_memory, not noise.
        hits = self.store.search(
            vector,
            limit=config.MEMORY_TOP_K,
            min_similarity=config.MEMORY_MIN_SIM,
            exclude_ids=self.conversation.tracked_ids(),
        )
        self.conversation.set_memory_block(memory.build_block(hits))
        return hits

    # -- internals ---------------------------------------------------------

    def _status(self, phase: str, text: str = "") -> None:
        # A warning is a note during boot, not a state of its own.
        if phase in ("boot", "ready", "error"):
            self.phase = phase
        self.emit(
            {
                "type": "status",
                "phase": phase,
                "text": text,
                "models": dict(self.models),
            }
        )


def _hit(hit: memory.Hit) -> Dict[str, Any]:
    """The wire shape of one recalled message."""
    return {
        "id": hit.id,
        "role": hit.role,
        "at": memory.format_local(hit.created_at),
        "score": round(hit.score, 4),
        "preview": " ".join(hit.content.split())[:70],
    }


def _query_text(text: str, previous_reply: Optional[str]) -> str:
    """The string to embed for retrieval.

    A short follow-up ("what about her?") needs the previous reply to carry
    any meaning, but a full sentence does not: concatenating the previous
    reply every time pulls retrieval back towards the topic the conversation
    just left, which is measured and wrong.
    """
    if previous_reply and len(text.split()) < config.MEMORY_QUERY_MIN_WORDS:
        return f"{previous_reply}\n{text}"
    return text


def _describe(call: dict) -> str:
    """A one-line account of what the model is looking up."""
    if call.get("name") != "search_memory":
        return f"calling {call.get('name')}"
    try:
        arguments = json.loads(call.get("arguments") or "{}")
    except json.JSONDecodeError:
        arguments = {}
    query = str(arguments.get("query") or "").strip()
    return f'searching "{query}"' if query else "searching"


def _assistant_message(text: str, calls: List[dict]) -> dict:
    """The assistant turn that requested the tool calls, as the API expects."""
    return {
        "role": "assistant",
        "content": text or None,
        "tool_calls": [
            {
                "id": call["id"],
                "type": "function",
                "function": {
                    "name": call["name"],
                    "arguments": call["arguments"],
                },
            }
            for call in calls
        ],
    }
