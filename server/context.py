"""Prompt assembly under a hard token budget.

Groq's free tier allows 6000 tokens/minute, so the prompt must stay small.
This module makes that a guarantee rather than a hope: history is dropped,
oldest first, until the estimate fits.
"""

Message = dict[str, str]


def estimate_tokens(text: str) -> int:
    """Approximate token count without pulling in a tokenizer.

    Latin text runs about 4 characters per token; Cyrillic is far worse on
    Llama-family tokenizers, closer to 2. Counting them separately keeps the
    estimate honest for the mixed-language traffic this server actually sees.
    The estimate is intentionally pessimistic — overshooting costs a little
    history, undershooting costs a 429.
    """
    if not text:
        return 0
    cyrillic = sum(1 for c in text if "Ѐ" <= c <= "ӿ")
    other = len(text) - cyrillic
    return int(cyrillic / 2 + other / 4) + 1


def build_messages(
    system: str,
    history: list[Message],
    user_text: str,
    budget: int,
) -> list[Message]:
    """Assemble the chat payload, trimming history to fit `budget` tokens.

    The system prompt and the current user message are never dropped: without
    them there is nothing to answer. If those two alone exceed the budget the
    caller gets them anyway and the request will be short by design.
    """
    floor = estimate_tokens(system) + estimate_tokens(user_text)
    room = budget - floor

    kept: list[Message] = []
    used = 0
    # Walk backwards so the most recent exchanges win the remaining room.
    for msg in reversed(history):
        cost = estimate_tokens(msg["content"])
        if used + cost > room:
            break
        kept.append(msg)
        used += cost
    kept.reverse()

    # A leading assistant message is a reply to a question that was just
    # trimmed away; it reads as non-sequitur context, so drop it too.
    while kept and kept[0]["role"] != "user":
        kept.pop(0)

    return [{"role": "system", "content": system}, *kept, {"role": "user", "content": user_text}]
