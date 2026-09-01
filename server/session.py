"""Per-connection state machine and the request pipeline.

One session is one WebSocket. The device is half-duplex: it either listens or
speaks, so the states are strictly sequential and audio arriving in the wrong
state is dropped rather than queued.
"""

import asyncio
import contextlib
import logging
import time
from enum import Enum

from server.context import build_messages, estimate_tokens
from server.costs import Usage
from server.codec import AdpcmDecoder, AdpcmEncoder
from server.emotion import LeadingTag, from_text, strip_tags
from server.lang import DEFAULT, detect_language, voice_for
from server.memory.summarise import extract_facts
from server.persona import ESP32, build_system_prompt
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
    def __init__(
        self, transport, stt, llm, tts, settings, store=None, device_id="default", embedder=None,
        style=ESP32,
    ):
        self.transport = transport
        self.stt = stt
        self.llm = llm
        self.tts = tts
        self.settings = settings
        self.store = store
        self.device_id = device_id
        self.embedder = embedder
        self.style = style

        self.state = State.IDLE
        self.history: list[dict] = []
        self.facts: list[str] = []
        self.standing_instructions: list[str] = []
        self.usage = Usage()

        self._buf = bytearray()
        self._codec = "pcm16"
        self._adpcm = None
        self._reply: asyncio.Task | None = None
        self._watchdog: asyncio.Task | None = None

    async def load_memory(self) -> None:
        if self.store is not None:
            self.standing_instructions = await self.store.user_facts(self.device_id)

    # -- events from the device -------------------------------------------

    async def on_start(self, codec: str = "pcm16") -> None:
        # A press during a reply is an interruption, not a mistake: stop
        # talking and listen. The device mutes itself before sending this, so
        # by the time it arrives the speaker is already quiet.
        if self.state in (State.THINKING, State.SPEAKING):
            await self.on_cancel()
        elif self.state is State.LISTENING:
            # A second "start" with no "end" between them, which the device
            # does not send idly: it means the utterance in progress was
            # abandoned on its side and is never going to be ended.
            #
            # This used to return here instead, and the silence was the whole
            # problem. The session stayed listening to a device that had
            # stopped sending, so every frame of the new question was accepted
            # into the old buffer, no reply was ever produced, and the first
            # thing anyone heard about it was "utterance timed out after
            # 60.0s" a minute later. Measured on 28 Aug: a start accepted
            # 0.19 s after a cancel, 0.22 s of audio, no "end", sixty seconds
            # of nothing. Starting cleanly costs the fragment, which was not
            # a question, and answers the one that was.
            log.info("start while already listening: dropping %d B never ended",
                     len(self._buf))
        self._buf.clear()
        # The device announces its codec per utterance. Absent the field it is
        # raw PCM, which keeps the laptop simulator working unchanged.
        self._codec = codec
        self._adpcm = AdpcmDecoder() if codec == "adpcm" else None
        await self._set_state(State.LISTENING)
        self._arm_watchdog()

    async def on_audio(self, chunk: bytes) -> None:
        if self.state is not State.LISTENING:
            return  # half-duplex: nothing to do with audio while replying

        if self._adpcm is not None:
            chunk = self._adpcm.feed(chunk)

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

        # The reply runs as its own task rather than inline.
        #
        # on_end is awaited from the socket's receive loop, so answering inline
        # meant nothing else could be read for the several seconds a reply
        # takes - including the request to stop talking. Detaching it keeps the
        # loop free to hear "cancel" while the reply is still being spoken.
        self._reply = asyncio.create_task(self._run_reply(lambda: self._respond(pcm)))

    async def _run_reply(self, coro_fn) -> None:
        try:
            await coro_fn()
        except asyncio.CancelledError:
            log.info("reply cancelled by the device")
            raise
        except Exception:
            log.exception("pipeline failed")
        finally:
            # The device has already stopped playing when it cancels, but it
            # still needs "done" to re-arm, and the socket may be gone.
            with contextlib.suppress(Exception):
                await self.transport.send_json({"type": "done"})
            with contextlib.suppress(Exception):
                await self._set_state(State.IDLE)

    async def wait_for_reply(self) -> None:
        """Block until the reply in progress finishes.

        The socket loop never needs this - it wants to stay free - but a caller
        driving the session directly, a test or the laptop simulator, does.
        """
        task = self._reply
        if task is not None:
            with contextlib.suppress(asyncio.CancelledError, Exception):
                await task

    async def on_cancel(self) -> None:
        """Stop the reply in progress. The user pressed the button to interrupt."""
        task, self._reply = self._reply, None
        if task is None or task.done():
            return
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError, Exception):
            await task

    async def on_text(self, text: str) -> None:
        """A typed question. No audio, no STT - it is already text."""
        if self.state is State.LISTENING:
            dropped = len(self._buf)
            self._cancel_watchdog()
            self._buf.clear()
            log.info("text arrived while listening: dropping %d B of audio in progress", dropped)
        elif self.state in (State.THINKING, State.SPEAKING):
            await self.on_cancel()
        await self._set_state(State.THINKING)
        self._reply = asyncio.create_task(self._run_reply(lambda: self._answer(text, speak=False)))

    async def finish(self) -> None:
        """Close out the session: extract durable facts, then log usage."""
        self._cancel_watchdog()
        await self.on_cancel()
        if self.store is None:
            return
        if self.history:
            facts = await extract_facts(self.llm, self.history)
            if facts:
                embeddings = await self.embedder.embed_documents(facts)
                await self.store.add_facts(self.device_id, facts, embeddings)
        if not self.usage.is_empty:
            await self.store.log_usage(self.device_id, self.usage)

    # -- pipeline ----------------------------------------------------------

    async def _respond(self, pcm: bytes) -> None:
        if not pcm:
            return  # nothing was captured; don't spend an STT call on silence

        # Whisper does not return nothing for a fragment of room tone. It
        # returns its training data: "Thank you.", "Спасибо.", "Продолжение
        # следует...". The model then answers those perfectly reasonably, so a
        # brushed button produced a device that said "Пожалуйста!" to
        # everything - observed in the logs, eight times in a row.
        #
        # The device cannot catch this on its own. It knows how long the button
        # was held; only this side knows how much audio actually arrived, and
        # under a second of it cannot be a question.
        seconds = len(pcm) / 2 / self.settings.sample_rate
        if seconds < self.settings.min_utterance_s:
            log.info("utterance of %.2f s is too short to be speech, not transcribing", seconds)
            return

        started = time.perf_counter()
        transcript = await self.stt.transcribe(pcm, self.settings.sample_rate)
        text = transcript.text.strip()
        if not text:
            log.info("empty transcript, skipping LLM")
            return
        log.info("stt %.0f ms: %s", (time.perf_counter() - started) * 1000, text)

        await self._answer(text, speak=True, audio_seconds=transcript.seconds)

    async def _answer(self, text: str, *, speak: bool = True, audio_seconds: float = 0.0) -> None:
        if self.store is not None:
            query_vector = await self.embedder.embed_query(text)
            self.facts = await self.store.relevant_facts(
                self.device_id, query_vector, limit=self.settings.relevant_facts_limit
            )

        messages = build_messages(
            # persona.py takes one flat list; standing instructions come
            # first so a relevant fact never pushes a user's own rule out of
            # the prompt if both were ever truncated upstream.
            build_system_prompt(self.standing_instructions + self.facts, self.style),
            self.history,
            text,
            self.settings.max_context_tokens,
        )
        prompt_tokens = sum(estimate_tokens(m["content"]) for m in messages)

        splitter = SentenceSplitter()
        # Sits ahead of the splitter: the model's feeling arrives as a tag on
        # the very first token, and nothing downstream should ever see it.
        tag = LeadingTag()
        spoken: list[str] = []
        voice: str | None = None
        language: str | None = None
        sent_bytes = 0
        reply_started = time.monotonic()
        # Reply audio goes back compressed too when the device asked for it.
        # A spoken answer is far larger than the question - 340 KB against
        # 15 KB - so the downlink benefits more from this than the uplink did.
        encoder = AdpcmEncoder() if self._codec == "adpcm" else None

        def is_speakable(sentence: str) -> bool:
            """True if there is anything for a voice to actually say.

            edge-tts raises NoAudioReceived for input with no pronounceable
            content - a stray bullet, a lone quotation mark, an emoji. One such
            fragment would otherwise abort the whole reply mid-sentence.
            """
            return any(ch.isalnum() for ch in sentence)

        async def say(sentence: str) -> None:
            nonlocal voice, language
            # Belt and braces. The sniffer takes the tag off the head of the
            # stream; this catches one the model put anywhere else, because
            # edge-tts will pronounce "curious" without hesitation.
            sentence = strip_tags(sentence)
            if not is_speakable(sentence):
                log.debug("skipping unspeakable fragment: %r", sentence)
                return
            if voice is None:
                # Decided once, from the first sentence: the voice must not
                # change partway through a reply.
                language = detect_language(sentence)
                voice = voice_for(language, self.settings.voices)
                # The face has to be right before the first word arrives, so
                # the emotion goes out ahead of the speaking state. By now the
                # tag has almost always resolved; when it has not, the first
                # sentence is a better thing to guess from than nothing.
                emotion = tag.emotion or from_text(sentence)
                await self.transport.send_json({"type": "emotion", "value": emotion})
                log.info("emotion %s (%s)", emotion,
                         "tagged" if tag.emotion else "guessed")
                await self._set_state(State.SPEAKING)
            spoken.append(sentence)

            if not speak:
                # Typed in, so written back - no TTS call spent on something
                # that is already being read.
                await self.transport.send_json({"type": "reply", "value": sentence})
                return

            async def render(with_voice: str) -> None:
                nonlocal sent_bytes
                loop = asyncio.get_running_loop()
                # Start on the short budget; relax it the moment audio arrives.
                async with asyncio.timeout(self.settings.tts_first_chunk_s) as limit:
                    started = False
                    async for pcm_chunk in self.tts.synthesise(sentence, with_voice):
                        if not started:
                            started = True
                            limit.reschedule(loop.time() + self.settings.tts_timeout_s)
                        # Pacing counts audio, not bytes on the wire, so the
                        # figure has to be taken before compression.
                        sent_bytes += len(pcm_chunk)
                        if encoder is not None:
                            pcm_chunk = encoder.feed(pcm_chunk)
                            if not pcm_chunk:
                                continue
                        await self.transport.send_bytes(pcm_chunk)

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
            for sentence in splitter.feed(tag.feed(delta)):
                await say(sentence)
        for sentence in splitter.feed(tag.flush()):
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
            audio_seconds=audio_seconds,
            prompt_tokens=prompt_tokens,
            completion_tokens=estimate_tokens(reply),
            tts_chars=sum(len(s) for s in spoken) if speak else 0,
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
