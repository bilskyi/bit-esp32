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


class FakeEmbedder:
    """No real embedding math - ranking correctness is tested against Store
    directly, with literal vectors. This only has to satisfy the interface."""

    async def embed_documents(self, texts: list[str]) -> list[list[float]]:
        return [[0.0] for _ in texts]

    async def embed_query(self, text: str) -> list[float]:
        return [0.0]


class FakeStore:
    def __init__(self, facts: list[str] | None = None, standing: list[str] | None = None) -> None:
        self.facts = {"default": list(facts or [])}
        self.standing = {"default": list(standing or [])}
        self.added: list[tuple[str, list[str]]] = []
        self.usage_logged: list = []
        self._memory: dict[str, list[dict]] = {}
        self._next_id = 1

    async def recent_facts(self, device_id: str, limit: int = 20) -> list[str]:
        return list(self.facts.get(device_id, []))

    async def relevant_facts(self, device_id: str, query_embedding, limit: int = 6) -> list[str]:
        # No ranking here: the fake ignores the query and returns whatever
        # was seeded, so pipeline tests don't need real embedding math.
        return list(self.facts.get(device_id, []))[:limit]

    async def user_facts(self, device_id: str) -> list[str]:
        return list(self.standing.get(device_id, []))

    async def add_facts(self, device_id: str, facts: list[str], embeddings=None) -> None:
        self.added.append((device_id, facts))

    async def add_user_fact(self, device_id: str, text: str, embedding=None) -> int:
        entry = {"id": self._next_id, "text": text, "source": "user", "created_at": None}
        self._next_id += 1
        self._memory.setdefault(device_id, []).append(entry)
        return entry["id"]

    async def list_memory(self, device_id: str) -> list[dict]:
        return list(self._memory.get(device_id, []))

    async def delete_fact(self, device_id: str, fact_id: int) -> bool:
        items = self._memory.get(device_id, [])
        before = len(items)
        self._memory[device_id] = [i for i in items if i["id"] != fact_id]
        return len(self._memory[device_id]) != before

    async def forget(self, device_id: str) -> None:
        self.facts[device_id] = []
        self.standing[device_id] = []
        self._memory[device_id] = []

    async def log_usage(self, device_id: str, usage) -> None:
        self.usage_logged.append((device_id, usage))


class SlowTTS:
    """Yields audio slowly, so a reply can be interrupted part-way through."""

    def __init__(self, chunks: int = 20, delay: float = 0.02) -> None:
        self.chunks = chunks
        self.delay = delay
        self.emitted = 0
        self.finished = 0

    async def synthesise(self, text: str, voice: str):
        import asyncio

        for _ in range(self.chunks):
            await asyncio.sleep(self.delay)
            self.emitted += 1
            yield b"\x11\x22" * 8
        self.finished += 1
