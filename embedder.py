"""Embedding client and server for long-term memory.

A second llama-server serves EmbeddingGemma 2 on its own port. Text is
task-prefixed (queries and documents differently), truncated to the model's 8K
window, and comes back as a 768-dimensional, L2-normalized vector.

Every call degrades to ``None`` instead of raising: memory is an enhancement,
and a broken embedder must never stop the chat.
"""

import math
from typing import Any, Callable, List, Optional

import requests

import config
import llama_server


def format_input(text: str, kind: str = "doc") -> str:
    """Apply the task prefix the model was trained with."""
    prefix = config.EMBED_QUERY_PREFIX if kind == "query" else config.EMBED_DOC_PREFIX
    return prefix + text[: config.EMBED_MAX_CHARS]


class Embedder:
    """HTTP client for the local embedding server."""

    def __init__(self, url: str = config.EMBED_URL) -> None:
        self.url = url
        self.model_name = config.EMBED_MODEL_NAME
        self._warned = False

    def is_up(self) -> bool:
        return llama_server.server_is_up(self.url)

    def embed(self, text: str, kind: str = "doc") -> Optional[List[float]]:
        """Embed one string, or return None when the server cannot answer."""
        if not text.strip():
            return None
        try:
            response = requests.post(
                f"{self.url}/v1/embeddings",
                json={"input": format_input(text, kind), "model": self.model_name},
                timeout=config.EMBED_TIMEOUT,
            )
            response.raise_for_status()
            vector = response.json()["data"][0]["embedding"]
        except (requests.RequestException, KeyError, IndexError, ValueError) as exc:
            self._warn(f"embedding request failed: {exc}")
            return None
        return self._normalize(vector)

    # -- internals ---------------------------------------------------------

    def _normalize(self, vector: Any) -> Optional[List[float]]:
        """Check the vector and scale it to unit length for cosine scoring."""
        values = [float(value) for value in vector]
        if len(values) != config.EMBED_DIM:
            self._warn(
                f"embedding returned {len(values)} dimensions, "
                f"expected {config.EMBED_DIM}"
            )
            return None
        if not all(math.isfinite(value) for value in values):
            self._warn(
                "embedding contains NaN or Inf; the model may be running in "
                "float16, which it does not support"
            )
            return None
        length = math.sqrt(sum(value * value for value in values))
        if length == 0.0:
            self._warn("embedding is all zeros")
            return None
        return [value / length for value in values]

    def _warn(self, message: str) -> None:
        """Report the first failure only; a dead embedder would spam otherwise."""
        if not self._warned:
            print(f"[warning] {message}")
            self._warned = True


def backfill(store: Any, embedder: Embedder, limit: int = 64) -> int:
    """Embed messages that were written while the embedder was unavailable."""
    filled = 0
    for message_id, content in store.pending(limit):
        vector = embedder.embed(content, "doc")
        if vector is None:
            break
        store.set_embedding(message_id, vector, embedder.model_name)
        filled += 1
    return filled


def embedding_server(
    log: Callable[[str], None] = print,
) -> llama_server.LlamaServer:
    """The EmbeddingGemma server, on its own port, text only."""
    return llama_server.LlamaServer(
        config.EMBED_MODEL_PATH,
        config.EMBED_PORT,
        label="embedding server",
        context_size=config.EMBED_CONTEXT_SIZE,
        extra_args=config.EMBED_EXTRA_ARGS,
        log_path=config.EMBED_LOG,
        log=log,
    )
