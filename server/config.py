"""Settings, loaded from the environment."""

from pydantic_settings import BaseSettings, SettingsConfigDict


class Settings(BaseSettings):
    model_config = SettingsConfigDict(env_file=".env", extra="ignore")

    groq_api_key: str = ""
    log_level: str = "info"

    # Groq's free tier allows 6000 tokens/minute. Prompt assembly is trimmed to
    # fit this; see server.context.
    max_context_tokens: int = 2000
    # A reply longer than a few sentences is unbearable read aloud.
    # The spec says ~150, which was right for a model that answers directly.
    # gpt-oss at 150 produces 250 characters - fifteen seconds of speech, during
    # which the device ignores the button and the link carries half a megabyte.
    # Ninety keeps replies near the two sentences the persona asks for.
    max_tokens: int = 90
    # Also bounds how much audio one utterance may buffer.
    session_timeout_s: float = 60.0

    # Below this, an utterance is not speech and is not worth an STT call.
    #
    # Whisper does not return an empty string for a fragment of room tone - it
    # returns its training data. Measured from the logs: 0.2-0.3 s fragments
    # came back as "Thank you." and "Спасибо.", which the model answered with
    # "Пожалуйста!", so a brushed button made the device say that to
    # everything. Nothing anyone actually asks fits in under a second: the
    # press, the words and the release all have to happen inside it.
    min_utterance_s: float = 1.0

    # Per-sentence ceiling on speech synthesis. edge-tts occasionally opens a
    # socket to Microsoft that never answers; without a bound that hangs the
    # whole reply, the device never receives "done", and it sits in SPEAKING
    # until its own watchdog fires. Observed in exactly that shape.
    tts_timeout_s: float = 10.0

    # Separate, much tighter budget for the first byte of a sentence.
    #
    # Measured over twelve draws: median 4.0 s, spread 2.2 to 5.7 s, with
    # occasional hangs past twenty seconds and occasional NoAudioReceived. An
    # earlier value of 3.0 sat below the median and turned five healthy draws
    # out of six into retries, which was far worse than the problem it was
    # meant to fix. Seven clears the observed spread and still catches a hang
    # well before the overall budget.
    tts_first_chunk_s: float = 7.0

    # How far ahead of real time the reply audio may be sent.
    #
    # Synthesis runs far faster than speech, so an unpaced reply arrives as a
    # flood: a 7-second answer lands in about two. The device has under a
    # second of buffer, so it had to push back by stalling its socket task,
    # which left the websocket client stuck mid-frame and killed the read.
    # Pacing here means it never has to. The first chunk is never delayed, so
    # time-to-first-audio is unaffected.
    # Fill the device's buffer, do not trickle into it.
    #
    # The buffer holds 48 KB of compressed audio - about six seconds. Pacing to
    # 1.2 s left five of those six unused, so a link that stalled for five
    # seconds starved playback anyway: one turn in twelve played 7.4 s of audio
    # over 17.4 s with 308 underruns. Four seconds of lead uses the buffer for
    # what it is for while staying clear of overfilling it.
    playback_lead_s: float = 4.0

    # Shared secret the device presents on the WebSocket handshake. Empty
    # disables the check, which is only appropriate on a laptop.
    device_token: str = ""

    # Signs the web login's session cookie. Must be set on Railway before
    # this deploys, same category as DEVICE_TOKEN and GROQ_API_KEY. Starlette
    # will still sign cookies with an empty key (fine for local dev and
    # tests), just not securely.
    session_secret_key: str = ""

    sample_rate: int = 16000
    db_path: str = "voice.db"

    # "groq" for real providers, "mock" for offline testing without an API key.
    provider_mode: str = "groq"

    stt_model: str = "whisper-large-v3-turbo"
    # Groq retired the Llama models; chat/completions returns 404 for them.
    # Measured alternatives (first token, uk/ru): gpt-oss-20b 845/420 ms,
    # gpt-oss-120b 550/530 ms. qwen3.6 is a reasoning model and emits <think>
    # blocks that would be spoken aloud, so it is not a candidate here.
    llm_model: str = "openai/gpt-oss-120b"

    voice_uk: str = "uk-UA-OstapNeural"
    voice_ru: str = "ru-RU-DmitryNeural"
    voice_en: str = "en-US-AndrewNeural"

    # Individual edge-tts voices intermittently return no audio for particular
    # phrases - observed with ru-RU-DmitryNeural on "Привет, Катерин!", which
    # ru-RU-SvetlanaNeural rendered without complaint. A second voice per
    # language turns that from a lost reply into a barely noticeable retry.
    voice_uk_alt: str = "uk-UA-PolinaNeural"
    voice_ru_alt: str = "ru-RU-SvetlanaNeural"
    voice_en_alt: str = "en-US-AvaNeural"

    # Covers uk/ru/en - this project's three languages - as ONNX via
    # fastembed, so no torch and no paid API. 0.22 GB quantized; the only
    # multilingual model in fastembed's registry small enough for Railway's
    # memory ceiling (multilingual-e5-large is 2.24 GB - checked, not guessed).
    embedding_model: str = "sentence-transformers/paraphrase-multilingual-MiniLM-L12-v2"
    # Empty uses fastembed's own cache location. On Railway, point this at
    # the same volume the database already uses (e.g. /data/fastembed_cache)
    # so a redeploy does not re-download the model.
    embedding_cache_dir: str = ""
    # How many auto-extracted facts the ranked retrieval hands to the prompt.
    relevant_facts_limit: int = 6

    @property
    def max_utterance_bytes(self) -> int:
        """Hard ceiling on one buffered utterance, in bytes of 16-bit PCM."""
        return int(self.session_timeout_s * self.sample_rate * 2)

    @property
    def auth_required(self) -> bool:
        return bool(self.device_token)

    @property
    def voices(self) -> dict[str, str]:
        return {"uk": self.voice_uk, "ru": self.voice_ru, "en": self.voice_en}

    @property
    def fallback_voices(self) -> dict[str, str]:
        return {
            "uk": self.voice_uk_alt,
            "ru": self.voice_ru_alt,
            "en": self.voice_en_alt,
        }
