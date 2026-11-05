"""Conversation state for the chat loop.

Keeps the system prompt and the user/assistant turns, and trims the oldest
turn pairs when the prompt would otherwise grow past its budget. This is a
cheap approximation of the server's own tokenizer: roughly four characters
per token, with the real limit still enforced by llama-server.

Each turn also carries the id of the row it was stored as in ``memory.db``, so
retrieval can skip messages that the model is already looking at.
"""

from typing import Dict, List, Optional

import config
import memory


class Conversation:
    def __init__(self, system_prompt: str = config.SYSTEM_PROMPT) -> None:
        self.system_prompt = system_prompt
        self.turns: List[Dict[str, str]] = []
        self.message_ids: List[Optional[int]] = []
        self.memory_block: str = ""

    def reset(self) -> None:
        """Forget everything except the persona and the retrieved memories."""
        self.turns.clear()
        self.message_ids.clear()

    def add_user(self, text: str, message_id: Optional[int] = None) -> None:
        self._append("user", text, message_id)

    def add_assistant(self, text: str, message_id: Optional[int] = None) -> None:
        self._append("assistant", text, message_id)

    def set_memory_block(self, block: str) -> None:
        """Replace the retrieved-memory section of the system prompt.

        An empty string removes the section entirely, leaving the prompt
        identical to a run with no memory.
        """
        self.memory_block = block

    def tracked_ids(self) -> set[int]:
        """Ids already in the live window, so retrieval does not repeat them."""
        return {
            message_id
            for message_id in self.message_ids
            if message_id is not None
        }

    def messages(self) -> List[Dict[str, str]]:
        """The full message list to send to the model."""
        self._trim_to_budget()
        return [{"role": "system", "content": self._system_text()}, *self.turns]

    # -- internals ---------------------------------------------------------

    def _append(self, role: str, text: str, message_id: Optional[int]) -> None:
        self.turns.append({"role": role, "content": text})
        self.message_ids.append(message_id)

    def _system_text(self) -> str:
        # The clock is read here, not at start-up: a session can run past
        # midnight, and a stale "today" would make every memory look older
        # than it is. The volatile parts go last so the persona stays a
        # stable prefix for the server's prompt cache.
        parts = [self.system_prompt, f"Today is {memory.now_local()}."]
        if self.memory_block:
            parts.append(self.memory_block)
        return "\n\n".join(parts)

    def _trim_to_budget(self) -> None:
        # Remove whole question/answer pairs, never the most recent one. The
        # memory block counts towards the budget, so a large block just leaves
        # less room for the live window.
        while (
            self._estimated_tokens() > config.MAX_HISTORY_TOKENS
            and len(self.turns) > 2
        ):
            del self.turns[:2]
            del self.message_ids[:2]

    def _estimated_tokens(self) -> int:
        characters = len(self._system_text()) + sum(
            len(message["content"]) for message in self.turns
        )
        return characters // 4
