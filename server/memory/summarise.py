"""End-of-session fact extraction.

After a conversation ends, the model is asked for a few durable facts about the
person. They are injected into the next session's system prompt, which is the
whole of the memory system: no embeddings, no vector store, no RAM cost.
"""

import json
import logging
import re

log = logging.getLogger(__name__)

MAX_FACTS = 5

INSTRUCTION = """Extract up to 5 durable facts about the user from this conversation.

A durable fact is something still true next week: their name, where they live,
what they work on, stable preferences. Do NOT include one-off questions, small
talk, weather, or anything about you.

Reply with a JSON array of short strings and nothing else. If there is nothing
worth remembering, reply with [].

Conversation:
"""


def _render(history: list[dict]) -> str:
    return "\n".join(f"{m['role']}: {m['content']}" for m in history)


def _parse(raw: str) -> list[str]:
    """Pull the first JSON array out of the reply and clean it up."""
    match = re.search(r"\[.*\]", raw, re.DOTALL)
    if not match:
        return []
    try:
        items = json.loads(match.group(0))
    except json.JSONDecodeError:
        log.debug("unparseable fact extraction: %r", raw[:200])
        return []
    if not isinstance(items, list):
        return []
    return [i.strip() for i in items if isinstance(i, str) and i.strip()][:MAX_FACTS]


async def extract_facts(llm, history: list[dict], max_tokens: int = 300) -> list[str]:
    """Ask the LLM for durable facts. Returns [] rather than raising."""
    if not history:
        return []
    messages = [{"role": "user", "content": INSTRUCTION + _render(history)}]
    try:
        raw = await llm.complete(messages, max_tokens)
    except Exception:
        log.exception("fact extraction failed")
        return []
    return _parse(raw)
