"""SQLite-backed long-term memory for the chat loop.

Stores every user and assistant message with a UTC timestamp and an embedding
vector. Retrieval is a cosine scan over the whole table followed by a recency
ordering and a token budget; at the scale of a personal chat that is fast
enough to do on every turn.

Memory is an enhancement, never a dependency: ``open_store()`` falls back to a
no-op store when the database cannot be opened, and the chat keeps running.
"""

import array
import sqlite3
import threading
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List, NamedTuple, Optional, Sequence, Tuple

import config

try:  # optional: only used to score the scan faster
    import numpy as _np
except ImportError:  # pragma: no cover - the pure-Python path still works
    _np = None

SCHEMA = """
CREATE TABLE IF NOT EXISTS messages (
    id           INTEGER PRIMARY KEY,
    session_id   TEXT    NOT NULL,
    role         TEXT    NOT NULL,
    content      TEXT    NOT NULL,
    created_at   TEXT    NOT NULL,
    token_est    INTEGER NOT NULL,
    embedding    BLOB,
    embed_model  TEXT
);
CREATE INDEX IF NOT EXISTS idx_messages_created ON messages(created_at);
CREATE INDEX IF NOT EXISTS idx_messages_pending ON messages(id)
    WHERE embedding IS NULL;
"""

ROLES = ("user", "assistant")


class Hit(NamedTuple):
    """One retrieved message."""

    id: int
    role: str
    content: str
    created_at: str
    score: float



def utc_timestamp() -> str:
    """UTC ISO-8601 to the second, so string order is also time order."""
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _render(moment: datetime) -> str:
    """One rendering for every timestamp a human or the model ever sees."""
    return f"{moment.isoformat(timespec='seconds')} ({moment.strftime('%a')})"


def format_local(created_at: str) -> str:
    """Turn a stored UTC timestamp into the local time shown to the model.

    The machine's own timezone is the reference, so a message sent late in the
    evening keeps the date the user was actually living in.
    """
    try:
        stored = datetime.strptime(created_at, "%Y-%m-%dT%H:%M:%SZ")
    except ValueError:
        return created_at  # never crash a turn over one odd row
    return _render(stored.replace(tzinfo=timezone.utc).astimezone())


def now_local() -> str:
    """The current instant, in the same format as ``format_local``."""
    return _render(datetime.now().astimezone())


def estimate_tokens(text: str) -> int:
    """The same approximation history.py uses: about four characters a token."""
    return len(text) // 4


class MessageStore:
    """The message table plus the connection that owns it."""

    def __init__(self, path: Path = config.MEMORY_DB) -> None:
        self.path = Path(path)
        # One connection is shared by the turn worker (writes) and the HTTP
        # handlers (reads, snapshots), so it cannot be bound to the thread
        # that happened to open it. A lock serialises use instead; turns are
        # already single-writer by construction.
        self._connection = sqlite3.connect(self.path, check_same_thread=False)
        self._lock = threading.RLock()
        self._connection.row_factory = sqlite3.Row
        self._connection.execute("PRAGMA journal_mode=WAL")
        self._connection.executescript(SCHEMA)
        self._connection.commit()

    def record(self, session_id: str, role: str, text: str) -> int:
        """Append one message and return its id."""
        if role not in ROLES:
            raise ValueError(f"unknown role: {role!r}")
        with self._lock:
            cursor = self._connection.execute(
                "INSERT INTO messages"
                " (session_id, role, content, created_at, token_est)"
                " VALUES (?, ?, ?, ?, ?)",
                (session_id, role, text, utc_timestamp(), estimate_tokens(text)),
            )
            self._connection.commit()
            return int(cursor.lastrowid)

    def peek(self, limit: int = 20) -> List[Dict[str, Any]]:
        """The most recent rows, newest first. For inspection, not the model."""
        with self._lock:
            cursor = self._connection.execute(
                "SELECT id, session_id, role, created_at, token_est, content"
                " FROM messages ORDER BY id DESC LIMIT ?",
                (limit,),
            )
            return [dict(row) for row in cursor]

    def count(self) -> int:
        with self._lock:
            cursor = self._connection.execute("SELECT COUNT(*) FROM messages")
            return int(cursor.fetchone()[0])

    def embedded_count(self) -> int:
        with self._lock:
            cursor = self._connection.execute(
                "SELECT COUNT(*) FROM messages WHERE embedding IS NOT NULL"
            )
            return int(cursor.fetchone()[0])

    def pending(self, limit: int = 64) -> List[Tuple[int, str]]:
        """Messages that still need an embedding, oldest first."""
        with self._lock:
            cursor = self._connection.execute(
                "SELECT id, content FROM messages"
                " WHERE embedding IS NULL ORDER BY id LIMIT ?",
                (limit,),
            )
            return [(int(row["id"]), row["content"]) for row in cursor]

    def set_embedding(
        self, message_id: int, vector: Sequence[float], model_name: str
    ) -> None:
        """Store one float32 vector."""
        blob = array.array("f", vector).tobytes()
        with self._lock:
            self._connection.execute(
                "UPDATE messages SET embedding = ?, embed_model = ? WHERE id = ?",
                (blob, model_name, message_id),
            )
            self._connection.commit()

    def embedding(self, message_id: int) -> Optional[List[float]]:
        """Read a stored vector back. For tests and debugging."""
        with self._lock:
            cursor = self._connection.execute(
                "SELECT embedding FROM messages WHERE id = ?", (message_id,)
            )
            row = cursor.fetchone()
        if row is None or row["embedding"] is None:
            return None
        values = array.array("f")
        values.frombytes(row["embedding"])
        return values.tolist()

    def search(
        self,
        vector: Sequence[float],
        limit: int = config.MEMORY_TOP_K,
        min_similarity: float = config.MEMORY_MIN_SIM,
        exclude_ids: Sequence[int] = (),
    ) -> List[Hit]:
        """The messages most similar to ``vector``, above the floor, best first."""
        excluded = set(exclude_ids)
        expected = config.EMBED_DIM * 4  # float32
        rows: List[sqlite3.Row] = []
        blobs: List[bytes] = []
        with self._lock:
            for row in self._connection.execute(
                "SELECT id, role, content, created_at, embedding FROM messages"
                " WHERE embedding IS NOT NULL"
            ):
                if row["id"] in excluded:
                    continue
                blob = row["embedding"]
                if len(blob) != expected:
                    continue  # written by a different model or dimension
                rows.append(row)
                blobs.append(blob)
        if not rows:
            return []

        hits = [
            Hit(int(row["id"]), row["role"], row["content"], row["created_at"], score)
            for row, score in zip(rows, _score(vector, blobs))
            if score >= min_similarity
        ]
        hits.sort(key=lambda hit: hit.score, reverse=True)
        return hits[:limit]

    def close(self) -> None:
        with self._lock:
            self._connection.close()


class NullStore:
    """Stand-in used when the database is unavailable."""

    def record(self, session_id: str, role: str, text: str) -> None:
        return None

    def peek(self, limit: int = 20) -> List[Dict[str, Any]]:
        return []

    def count(self) -> int:
        return 0

    def embedded_count(self) -> int:
        return 0

    def pending(self, limit: int = 64) -> List[Tuple[int, str]]:
        return []

    def set_embedding(
        self, message_id: int, vector: Sequence[float], model_name: str
    ) -> None:
        return None

    def embedding(self, message_id: int) -> Optional[List[float]]:
        return None

    def search(self, *args: Any, **kwargs: Any) -> List[Hit]:
        return []

    def close(self) -> None:
        return None


def _score(vector: Sequence[float], blobs: Sequence[bytes]) -> List[float]:
    """Cosine similarity of one unit vector against stored unit vectors."""
    if _np is not None:
        matrix = _np.vstack([_np.frombuffer(blob, dtype=_np.float32) for blob in blobs])
        return (_np.asarray(vector, dtype=_np.float32) @ matrix.T).tolist()

    scores = []
    for blob in blobs:
        values = array.array("f")
        values.frombytes(blob)
        scores.append(sum(a * b for a, b in zip(vector, values)))
    return scores


def order_hits(
    hits: Sequence[Hit], newest_first: Optional[bool] = None
) -> List[Hit]:
    """Sort by recency; this is the order the block is written in."""
    if newest_first is None:
        newest_first = config.MEMORY_ORDER != "oldest_first"
    return sorted(
        hits,
        key=lambda hit: (hit.created_at, hit.id),
        reverse=newest_first,
    )


def render_line(hit: Hit) -> str:
    """One message in the format every surface uses, block and tool alike."""
    return (
        f"[{format_local(hit.created_at)}] {_speaker(hit.role)}: "
        f"{_one_line(hit.content)}"
    )


def build_block(
    hits: Sequence[Hit], max_tokens: int = config.MAX_MEMORY_TOKENS
) -> str:
    """Render hits as the memory section of the system prompt.

    Returns an empty string when nothing fits, so a turn with no relevant
    memory produces a prompt identical to one from before memory existed.
    """
    if not hits:
        return ""

    header = config.MEMORY_HEADER
    lines = [header]
    used = estimate_tokens(header)
    for hit in order_hits(hits):
        line = render_line(hit)
        cost = estimate_tokens(line)
        if used + cost > max_tokens:
            continue  # skip this one; shorter or newer items may still fit
        lines.append(line)
        used += cost

    if len(lines) == 1:
        return ""
    return "\n".join(lines)


def _speaker(role: str) -> str:
    return "user" if role == "user" else "Hashta"


def _one_line(text: str) -> str:
    """Collapse whitespace and cut long messages so one item is one line."""
    text = " ".join(text.split())
    if len(text) <= config.MEMORY_ITEM_MAX_CHARS:
        return text
    return text[: config.MEMORY_ITEM_MAX_CHARS].rstrip() + " ..."


def open_store(path: Optional[Path] = None) -> Any:
    """Open the database, or return a no-op store if that fails."""
    try:
        return MessageStore(path if path is not None else config.MEMORY_DB)
    except sqlite3.Error as exc:
        print(f"[warning] long-term memory disabled: {exc}")
        return NullStore()


if __name__ == "__main__":
    store = open_store()
    rows = store.peek(20)
    print(f"{store.count()} messages in {config.MEMORY_DB}")
    for row in reversed(rows):
        print(
            f"{row['id']:>5}  {format_local(row['created_at'])}  "
            f"{row['role']:<9} {row['token_est']:>4}t  {row['content'][:60]!r}"
        )
    store.close()
