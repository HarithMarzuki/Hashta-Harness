"""A small client for llama-server's OpenAI-compatible chat endpoint.

The reply is streamed so the terminal can show tokens as they are produced.
Only the plain text content is surfaced; reasoning tokens, if the model emits
any, are handled by the server's chat template.

A turn is a sequence of events rather than a plain string stream, because the
model may answer with tool calls instead of text:

    ("text", "Hello")            one piece of the visible reply
    ("tool_calls", [ {...}, ])   the model wants the harness to look something
                                 up before it can answer

Tool-call fragments arrive spread over many chunks (id, then name, then the
arguments in pieces), so they are accumulated by index and reported once the
stream ends.
"""

import json
from typing import Any, Dict, Iterator, List, Optional, Tuple

import requests

import config


class LlmError(RuntimeError):
    """Raised when the server rejects a request or the stream breaks."""


Event = Tuple[str, Any]


def stream_turn(
    messages: List[dict],
    tools: Optional[List[dict]] = None,
    **overrides: Any,
) -> Iterator[Event]:
    """Send ``messages`` and yield ("text", piece) and, at the end, tool calls."""
    payload: Dict[str, Any] = {
        "messages": messages,
        "stream": True,
        "temperature": overrides.get("temperature", config.TEMPERATURE),
        "top_k": overrides.get("top_k", config.TOP_K),
        "repeat_penalty": overrides.get(
            "repeat_penalty", config.REPETITION_PENALTY
        ),
        "max_tokens": overrides.get("max_tokens", config.MAX_TOKENS),
    }
    if tools:
        payload["tools"] = tools
        payload["tool_choice"] = overrides.get("tool_choice", "auto")

    url = f"{config.SERVER_URL}/v1/chat/completions"
    partial: Dict[int, Dict[str, str]] = {}

    try:
        with requests.post(
            url, json=payload, stream=True, timeout=(10, 300)
        ) as response:
            if response.status_code != 200:
                raise LlmError(
                    f"Server returned HTTP {response.status_code}: "
                    f"{response.text[:500]}"
                )

            # Read the raw bytes and decode them as UTF-8 ourselves. Asking
            # requests to decode (decode_unicode=True) would guess ISO-8859-1
            # from the ``text/event-stream`` content type and turn every emoji
            # or non-Latin character into mojibake. Splitting on b"\n" is safe
            # because no continuation byte of a UTF-8 sequence can be 0x0A.
            for raw_line in response.iter_lines():
                if not raw_line:
                    continue
                line = raw_line.decode("utf-8", errors="replace")
                # Server-sent events look like:  data: {json}\n\n
                if not line.startswith("data: "):
                    continue
                data = line[len("data: "):]
                if data == "[DONE]":
                    break

                chunk = json.loads(data)
                choices = chunk.get("choices") or []
                if not choices:
                    continue
                delta = choices[0].get("delta") or {}

                text = delta.get("content")
                if text:
                    yield ("text", text)

                for fragment in delta.get("tool_calls") or []:
                    _collect(partial, fragment)

    except requests.RequestException as exc:
        raise LlmError(
            f"Could not reach llama-server at {config.SERVER_URL}: {exc}"
        ) from exc

    if partial:
        yield ("tool_calls", [_finished(partial[i]) for i in sorted(partial)])


def _collect(partial: Dict[int, Dict[str, str]], fragment: dict) -> None:
    """Merge one streamed tool-call fragment into the accumulated call."""
    index = int(fragment.get("index", 0))
    call = partial.setdefault(index, {"id": "", "name": "", "arguments": ""})
    if fragment.get("id"):
        call["id"] = fragment["id"]
    function = fragment.get("function") or {}
    if function.get("name"):
        call["name"] = function["name"]
    if function.get("arguments"):
        call["arguments"] += function["arguments"]


def _finished(call: Dict[str, str]) -> dict:
    """The shape the harness passes back to the server and to the toolbox."""
    return {
        "id": call["id"] or "call_0",
        "name": call["name"],
        "arguments": call["arguments"],
    }
