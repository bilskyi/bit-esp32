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

    # Shared secret the device presents on the WebSocket handshake. Empty
    # disables the check, which is only appropriate on a laptop.
    device_token: str = ""

    sample_rate: int = 16000
    db_path: str = "voice.db"

    # "groq" for real providers, "mock" for offline testing without an API key.
    provider_mode: str = "groq"

    stt_model: str = "whisper-large-v3-turbo"
    llm_model: str = "llama-3.3-70b-versatile"

    voice_uk: str = "uk-UA-OstapNeural"
    voice_ru: str = "ru-RU-DmitryNeural"
    voice_en: str = "en-US-AndrewNeural"

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
