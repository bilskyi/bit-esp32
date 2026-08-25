"""Per-connection state machine and the request pipeline.

One session is one WebSocket. The device is half-duplex: it either listens or
speaks, so the states are strictly sequential and audio arriving in the wrong
state is dropped rather than queued.
"""

import asyncio
import logging
import time
from enum import Enum

from server.context import build_messages, estimate_tokens
from server.costs import Usage
from server.lang import detect_language, voice_for
from server.memory.summarise import extract_facts
from server.persona import build_system_prompt
from server.sentences import SentenceSplitter

log = logging.getLogger(__name__)


class State(str, Enum):
    IDLE = "idle"
    LISTENING = "listening"
    THINKING = "thinking"
    SPEAKING = "speaking"


class Session:
    def __init__(self, transport, stt, llm, tts, settings, store=None, device_id="default"):
        self.transport = transport
        self.stt = stt
        self.llm = llm
        self.tts = tts
        self.settings = settings
        self.store = store
        self.device_id = device_id

        self.state = State.IDLE
        self.history: list[dict] = []
        self.facts: list[str] = []
        self.usage = Usage()

        self._buf = bytearray()
        self._watchdog: asyncio.Task | None = None

    async def load_memory(self) -> None:
        if self.store is not None:
            self.facts = await self.store.recent_facts(self.device_id)

    # -- events from the device -------------------------------------------

    async def on_start(self) -> None:
        if self.state is not State.IDLE:
            log.debug("ignoring start in state %s", self.state)
            return
        self._buf.clear()
        await self._set_state(State.LISTENING)
        self._arm_watchdog()

    async def on_audio(self, chunk: bytes) -> None:
        if self.state is not State.LISTENING:
            return  # half-duplex: nothing to do with audio while replying
        room = self.settings.max_utterance_bytes - len(self._buf)
        if room <= 0:
            return
        self._buf += chunk[:room]

    async def on_end(self) -> None:
        if self.state is not State.LISTENING:
            return
        self._cancel_watchdog()
        await self._set_state(State.THINKING)
        pcm, self._buf = bytes(self._buf), bytearray()
        try:
            await self._respond(pcm)
        except Exception:
            log.exception("pipeline failed")
        finally:
            await self.transport.send_json({"type": "done"})
            await self._set_state(State.IDLE)

    async def finish(self) -> None:
        """Close out the session: extract durable facts, then log usage."""
        self._cancel_watchdog()
        if self.store is None:
            return
        if self.history:
            facts = await extract_facts(self.llm, self.history)
            if facts:
                await self.store.add_facts(self.device_id, facts)
        if not self.usage.is_empty:
            await self.store.log_usage(self.device_id, self.usage)

    # -- pipeline ----------------------------------------------------------

    async def _respond(self, pcm: bytes) -> None:
        if not pcm:
            return  # nothing was captured; don't spend an STT call on silence

        started = time.perf_counter()
        transcript = await self.stt.transcribe(pcm, self.settings.sample_rate)
        text = transcript.text.strip()
        if not text:
            log.info("empty transcript, skipping LLM")
            return
        log.info("stt %.0f ms: %s", (time.perf_counter() - started) * 1000, text)

        messages = build_messages(
            build_system_prompt(self.facts),
            self.history,
            text,
            self.settings.max_context_tokens,
        )
        prompt_tokens = sum(estimate_tokens(m["content"]) for m in messages)

        splitter = SentenceSplitter()
        spoken: list[str] = []
        voice: str | None = None

        async def say(sentence: str) -> None:
            nonlocal voice
            if voice is None:
                # Decided once, from the first sentence: the voice must not
                # change partway through a reply.
                voice = voice_for(detect_language(sentence), self.settings.voices)
                await self._set_state(State.SPEAKING)
            spoken.append(sentence)
            async for pcm_chunk in self.tts.synthesise(sentence, voice):
                await self.transport.send_bytes(pcm_chunk)

        async for delta in self.llm.stream(messages, self.settings.max_tokens):
            for sentence in splitter.feed(delta):
                await say(sentence)
        for sentence in splitter.flush():
            await say(sentence)

        reply = " ".join(spoken)
        self.history.append({"role": "user", "content": text})
        self.history.append({"role": "assistant", "content": reply})
        self.usage.add_turn(
            audio_seconds=transcript.seconds,
            prompt_tokens=prompt_tokens,
            completion_tokens=estimate_tokens(reply),
            tts_chars=sum(len(s) for s in spoken),
        )

    # -- helpers -----------------------------------------------------------

    async def _set_state(self, state: State) -> None:
        self.state = state
        await self.transport.send_json({"type": "state", "value": state.value})

    def _arm_watchdog(self) -> None:
        self._cancel_watchdog()
        self._watchdog = asyncio.create_task(self._timeout())

    def _cancel_watchdog(self) -> None:
        if self._watchdog is not None:
            self._watchdog.cancel()
            self._watchdog = None

    async def _timeout(self) -> None:
        """A held or stuck button must not stream silence forever."""
        try:
            await asyncio.sleep(self.settings.session_timeout_s)
        except asyncio.CancelledError:
            return
        log.warning("utterance timed out after %ss", self.settings.session_timeout_s)
        self._watchdog = None
        await self.on_end()
