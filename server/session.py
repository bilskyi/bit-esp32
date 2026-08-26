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
from server.lang import DEFAULT, detect_language, voice_for
from server.memory.summarise import extract_facts
from server.persona import build_system_prompt
from server.sentences import SentenceSplitter

log = logging.getLogger(__name__)


# Said aloud when the model returns nothing, which happens when the transcript
# is garbled - usually because the upload was truncated by a stalled link.
_DIDNT_CATCH = {
    "uk": "Вибач, я не розчув. Повтори, будь ласка.",
    "ru": "Извини, я не расслышал. Повтори, пожалуйста.",
    "en": "Sorry, I did not catch that. Could you say it again?",
}


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

        # DEBUG_DUMP_PCM=1 writes each received utterance to disk so the audio
        # that actually crossed the network can be inspected. Bench aid only.
        import os

        if os.getenv("DEBUG_DUMP_PCM"):
            from server.audio import pcm_to_wav

            path = f"/tmp/utterance_{int(len(pcm))}.wav"
            with open(path, "wb") as fh:
                fh.write(pcm_to_wav(pcm, self.settings.sample_rate))
            log.info("dumped %d bytes (%.2f s) -> %s",
                     len(pcm), len(pcm) / 2 / self.settings.sample_rate, path)

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
        language: str | None = None
        sent_bytes = 0
        reply_started = time.monotonic()

        def is_speakable(sentence: str) -> bool:
            """True if there is anything for a voice to actually say.

            edge-tts raises NoAudioReceived for input with no pronounceable
            content - a stray bullet, a lone quotation mark, an emoji. One such
            fragment would otherwise abort the whole reply mid-sentence.
            """
            return any(ch.isalnum() for ch in sentence)

        async def say(sentence: str) -> None:
            nonlocal voice, language
            if not is_speakable(sentence):
                log.debug("skipping unspeakable fragment: %r", sentence)
                return
            if voice is None:
                # Decided once, from the first sentence: the voice must not
                # change partway through a reply.
                language = detect_language(sentence)
                voice = voice_for(language, self.settings.voices)
                await self._set_state(State.SPEAKING)
            spoken.append(sentence)

            async def render(with_voice: str) -> None:
                nonlocal sent_bytes
                async with asyncio.timeout(self.settings.tts_timeout_s):
                    async for pcm_chunk in self.tts.synthesise(sentence, with_voice):
                        await self.transport.send_bytes(pcm_chunk)
                        sent_bytes += len(pcm_chunk)

                        # Stay at most playback_lead_s ahead of what the device
                        # can have played by now.
                        bytes_per_second = self.settings.sample_rate * 2
                        audio_sent = sent_bytes / bytes_per_second
                        elapsed = time.monotonic() - reply_started
                        ahead = audio_sent - elapsed
                        if ahead > self.settings.playback_lead_s:
                            await asyncio.sleep(ahead - self.settings.playback_lead_s)

            try:
                await render(voice)
                return
            except Exception:
                log.warning("tts failed on %s, retrying with fallback voice", voice)

            # Second attempt with the other voice for this language. The
            # failure is per voice and per phrase, not per language, so the
            # alternate usually renders the same text without trouble.
            alt = self.settings.fallback_voices.get(language or "uk")
            if not alt or alt == voice:
                return
            try:
                await render(alt)
            except Exception:
                # One sentence the voice cannot render must not silence the
                # rest of the reply. edge-tts raises when the text does not
                # match the voice's language, which happens whenever the STT
                # misfires and the model answers in a language we did not pick
                # a voice for.
                log.warning("tts failed for %r, skipping sentence", sentence[:60])

        async for delta in self.llm.stream(messages, self.settings.max_tokens):
            for sentence in splitter.feed(delta):
                await say(sentence)
        for sentence in splitter.flush():
            await say(sentence)

        if not spoken:
            # The model answers a garbled transcript with an empty string, and
            # an empty reply reaches the user as unexplained silence - which is
            # indistinguishable from the device being broken. Say so instead.
            # Do not trust the language of a transcript we already know is
            # garbled: "Raskarji, Karla." looks like English and is not. The
            # last reply that actually made sense is a far better guide.
            prior = next(
                (m["content"] for m in reversed(self.history) if m["role"] == "assistant"),
                "",
            )
            lang = detect_language(prior) if prior else DEFAULT
            fallback = _DIDNT_CATCH.get(lang, _DIDNT_CATCH[DEFAULT])
            log.info("empty reply for %r, asking to repeat", text[:40])
            await say(fallback)

        reply = " ".join(spoken)
        log.info(
            "reply %d chars, %d sentences, %d B audio in %.1f s: %r",
            len(reply), len(spoken), sent_bytes,
            time.monotonic() - reply_started, reply[:80],
        )
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
