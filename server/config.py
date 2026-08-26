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
    max_tokens: int = 150
    # Also bounds how much audio one utterance may buffer.
    session_timeout_s: float = 60.0

    # Per-sentence ceiling on speech synthesis. edge-tts occasionally opens a
    # socket to Microsoft that never answers; without a bound that hangs the
    # whole reply, the device never receives "done", and it sits in SPEAKING
    # until its own watchdog fires. Observed in exactly that shape.
    tts_timeout_s: float = 10.0

    # How far ahead of real time the reply audio may be sent.
    #
    # Synthesis runs far faster than speech, so an unpaced reply arrives as a
    # flood: a 7-second answer lands in about two. The device has under a
    # second of buffer, so it had to push back by stalling its socket task,
    # which left the websocket client stuck mid-frame and killed the read.
    # Pacing here means it never has to. The first chunk is never delayed, so
    # time-to-first-audio is unaffected.
    # Must stay under the device's play buffer, which holds 24576 bytes
    # = 0.77 s. A larger lead simply overfills it and forces the device to
    # stall its socket task, which is the problem pacing was meant to avoid.
    playback_lead_s: float = 0.4

    # Shared secret the device presents on the WebSocket handshake. Empty
    # disables the check, which is only appropriate on a laptop.
    device_token: str = ""

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
