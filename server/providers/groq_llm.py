"""Groq chat completion, streamed."""

import asyncio
import json
import logging
from typing import AsyncIterator, Callable

import httpx

from server.providers._retry import RETRY_STATUSES, retry_after

log = logging.getLogger(__name__)

BASE_URL = "https://api.groq.com/openai/v1/chat/completions"


class GroqLLM:
    def __init__(
        self,
        api_key: str,
        model: str,
        client: httpx.AsyncClient | None = None,
        max_retries: int = 4,
        sleep: Callable = asyncio.sleep,
        timeout: float = 30.0,
        reasoning_effort: str = "low",
    ) -> None:
        self._key = api_key
        self._model = model
        self._client = client or httpx.AsyncClient(timeout=timeout)
        self._max_retries = max_retries
        self._sleep = sleep
        self._reasoning_effort = reasoning_effort

    def _payload(self, messages: list[dict], max_tokens: int, stream: bool) -> dict:
        return {
            "model": self._model,
            "messages": messages,
            "max_tokens": max_tokens,
            "temperature": 0.7,
            "stream": stream,
            # gpt-oss reasons before it answers, and the reasoning is billed
            # against the same budget. Measured on "Расскажи мне про космос."
            # at max_tokens=150: the default effort spent 511 characters
            # thinking and emitted 12 of answer; "low" spent 64 and emitted
            # 359. Every unanswered open question - turbulence, space - was
            # this, not the network.
            "reasoning_effort": self._reasoning_effort,
        }

    @property
    def _headers(self) -> dict:
        return {"Authorization": f"Bearer {self._key}", "Content-Type": "application/json"}

    async def stream(self, messages: list[dict], max_tokens: int) -> AsyncIterator[str]:
        """Yield reply deltas. Retries are handled before the body is consumed."""
        for attempt in range(self._max_retries):
            request = self._client.build_request(
                "POST", BASE_URL, json=self._payload(messages, max_tokens, True), headers=self._headers
            )
            response = await self._client.send(request, stream=True)
            if response.status_code in RETRY_STATUSES and attempt < self._max_retries - 1:
                delay = retry_after(response, attempt)
                log.warning("LLM HTTP %s, retrying in %.2fs", response.status_code, delay)
                await response.aclose()
                await self._sleep(delay)
                continue
            try:
                response.raise_for_status()
                async for line in response.aiter_lines():
                    if not line.startswith("data: "):
                        continue
                    body = line[6:].strip()
                    if body == "[DONE]":
                        return
                    try:
                        chunk = json.loads(body)
                    except json.JSONDecodeError:
                        log.debug("skipping malformed SSE line")
                        continue
                    content = chunk.get("choices", [{}])[0].get("delta", {}).get("content")
                    if content:
                        yield content
            finally:
                await response.aclose()
            return

    async def complete(self, messages: list[dict], max_tokens: int) -> str:
        """Non-streaming completion, used for memory extraction."""
        from server.providers._retry import with_retries

        async def send():
            return await self._client.post(
                BASE_URL, json=self._payload(messages, max_tokens, False), headers=self._headers
            )

        response = await with_retries(send, self._max_retries, self._sleep)
        return response.json()["choices"][0]["message"]["content"]
