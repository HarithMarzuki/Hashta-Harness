"""Tools the model may call while composing a reply.

One tool for now: ``search_memory``. The block the harness already placed in
the prompt is the signpost; this is how the model follows one it noticed.

The toolbox never raises. Every failure comes back as text the model can read
and react to, so a bad call degrades the answer instead of ending the turn.
"""

import json
from typing import Any, Optional, Set

import config
import memory

SEARCH_MEMORY = {
    "type": "function",
    "function": {
        "name": "search_memory",
        "description": (
            "Search earlier conversations with the user. Use it when the "
            "background mentions something worth following up, or when you "
            "need a detail you do not have."
        ),
        "parameters": {
            "type": "object",
            "properties": {
                "query": {
                    "type": "string",
                    "description": "What to look for, in natural language.",
                },
                "limit": {
                    "type": "integer",
                    "description": "How many messages to return, 1 to 10.",
                },
            },
            "required": ["query"],
        },
    },
}

TOOLS = [SEARCH_MEMORY]

UNAVAILABLE = "Memory is unavailable right now."


class Toolbox:
    """Runs one turn's tool calls and keeps them inside their budget."""

    def __init__(
        self,
        store: Any,
        embedder: Optional[Any],
        seen_ids: Set[int],
        budget: int = config.MEMORY_TOOL_BUDGET,
    ) -> None:
        self.store = store
        self.embedder = embedder
        self.seen_ids = set(seen_ids)
        self.budget = budget

    def call(self, name: str, arguments: str) -> str:
        if name != "search_memory":
            return (
                f"Unknown tool {name!r}. The only tool available is "
                f"search_memory."
            )
        return self._search(arguments)

    # -- search ------------------------------------------------------------

    def _search(self, arguments: str) -> str:
        if self.budget <= 0:
            return "Memory lookups are exhausted for this turn."

        parsed = _parse(arguments)
        if parsed is None:
            return (
                'The arguments were not valid JSON. Call search_memory with '
                '{"query": "..."}.'
            )
        query = str(parsed.get("query") or "").strip()
        if not query:
            return 'search_memory needs a non-empty "query".'
        if self.embedder is None:
            return UNAVAILABLE

        limit = _limit(parsed.get("limit"))
        vector = self.embedder.embed(query, "query")
        if vector is None:
            return UNAVAILABLE

        # No similarity floor: the model asked on purpose, so a weak match is
        # more useful than silence. The model can see it is weak.
        hits = self.store.search(
            vector,
            limit=limit,
            min_similarity=0.0,
            exclude_ids=self.seen_ids,
        )
        if not hits:
            return f'No earlier messages matched "{query}".'

        lines = [f'{len(hits)} earlier message(s) for "{query}":']
        for hit in memory.order_hits(hits):
            lines.append(memory.render_line(hit))
            self.seen_ids.add(hit.id)
        text = "\n".join(lines)
        self.budget -= memory.estimate_tokens(text)
        return text


def _parse(arguments: str) -> Optional[dict]:
    try:
        parsed = json.loads(arguments) if arguments.strip() else {}
    except json.JSONDecodeError:
        return None
    return parsed if isinstance(parsed, dict) else None


def _limit(value: Any) -> int:
    try:
        wanted = int(value)
    except (TypeError, ValueError):
        return config.MEMORY_TOOL_LIMIT
    return max(1, min(wanted, config.MEMORY_TOOL_LIMIT_MAX))
