"""In-memory doubles so the whole pipeline runs with no network and no device."""

from server.providers.base import Transcript


class FakeTransport:
    def __init__(self) -> None:
        self.json: list[dict] = []
        self.binary: list[bytes] = []

    async def send_json(self, obj: dict) -> None:
        self.json.append(obj)

    async def send_bytes(self, data: bytes) -> None:
        self.binary.append(data)

    @property
    def states(self) -> list[str]:
        return [m["value"] for m in self.json if m.get("type") == "state"]

    @property
    def types(self) -> list[str]:
        return [m["type"] for m in self.json]


class FakeSTT:
    def __init__(self, text: str = "Як справи?") -> None:
        self.text = text
        self.received: list[int] = []

    async def transcribe(self, pcm: bytes, sample_rate: int) -> Transcript:
        self.received.append(len(pcm))
        return Transcript(text=self.text, language="uk", seconds=len(pcm) / 2 / sample_rate)


class FakeLLM:
    def __init__(self, reply: str = "Все добре. А в тебе як?") -> None:
        self.reply = reply
        self.prompts: list[list[dict]] = []
        self.completions: list[str] = []

    async def stream(self, messages, max_tokens):
        self.prompts.append(messages)
        for word in self.reply.split(" "):
            yield word + " "

    async def complete(self, messages, max_tokens) -> str:
        self.prompts.append(messages)
        return self.completions.pop(0) if self.completions else self.reply


class FakeTTS:
    def __init__(self) -> None:
        self.spoken: list[tuple[str, str]] = []

    async def synthesise(self, text: str, voice: str):
        self.spoken.append((text, voice))
        yield b"\x11\x22" * 8
        yield b"\x33\x44" * 8

    @property
    def voices(self) -> list[str]:
        return [v for _, v in self.spoken]
