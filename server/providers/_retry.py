"""Shared 429/5xx backoff.

Groq's free tier allows 20 RPM for STT and 30 RPM for LLM. Hitting the limit is
expected under normal use, so retrying is part of the happy path, not error
handling.
"""

import asyncio
import logging
import random
from typing import Awaitable, Callable

import httpx

log = logging.getLogger(__name__)

RETRY_STATUSES = frozenset({408, 429, 500, 502, 503, 504})


def retry_after(response: httpx.Response, attempt: int) -> float:
    """Honour Retry-After when present, else exponential backoff with jitter."""
    header = response.headers.get("retry-after")
    if header:
        try:
            return max(0.0, float(header))
        except ValueError:
            pass
    return min(8.0, 0.5 * (2**attempt)) + random.uniform(0, 0.25)


async def with_retries(
    send: Callable[[], Awaitable[httpx.Response]],
    max_retries: int,
    sleep: Callable[[float], Awaitable[None]] = asyncio.sleep,
) -> httpx.Response:
    """Call `send` until it returns a non-retryable response or attempts run out."""
    for attempt in range(max_retries):
        response = await send()
        if response.status_code not in RETRY_STATUSES:
            response.raise_for_status()
            return response
        if attempt == max_retries - 1:
            break
        delay = retry_after(response, attempt)
        log.warning("HTTP %s, retrying in %.2fs", response.status_code, delay)
        await response.aclose()
        await sleep(delay)
    response.raise_for_status()
    return response
