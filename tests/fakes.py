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


class FakeRoles:
    """Lets a test change a surface's active role mid-connection, the way a
    real Roles.set_active() would, without a database."""

    def __init__(self, role) -> None:
        self._role = role

    async def active_for(self, surface: str):
        return self._role

    def switch_to(self, role) -> None:
        self._role = role


class FakeAccounts:
    def __init__(self, username: str = "test", password: str = "test123") -> None:
        self._username = username
        self._password = password

    async def verify_password(self, username: str, password: str) -> bool:
        return username == self._username and password == self._password


class FakeStore:
    def __init__(
        self, facts: list[str] | None = None, standing: list[str] | None = None,
        store_conversations: bool = True, retention_days: int = 90,
        conversations: list[dict] | None = None, usage: list[dict] | None = None,
    ) -> None:
        self.facts = {"default": list(facts or [])}
        self.standing = {"default": list(standing or [])}
        self.added: list[tuple[str, list[str]]] = []
        self.usage_logged: list = []
        self._memory: dict[str, list[dict]] = {}
        self._next_id = 1
        self.turns: list[tuple[str, str, str | None]] = []
        self.started: list[int] = []
        self.ended: list[int] = []
        self.deleted_conversations_for: list[str] = []
        self._store_conversations = store_conversations
        self._retention_days = retention_days
        # Seeded rows for the two Task 5 read endpoints - "default" is the
        # one device_id every test (and the app itself) actually uses, same
        # convention as self.facts/self.standing above.
        self._conversations = {"default": list(conversations or [])}
        self._usage = {"default": list(usage or [])}

    async def conversation_rows(self, device_id: str, limit: int = 50) -> list[dict]:
        return list(self._conversations.get(device_id, []))[:limit]

    async def usage_rows(self, device_id: str, limit: int = 50) -> list[dict]:
        return list(self._usage.get(device_id, []))[:limit]

    async def app_settings(self) -> dict:
        return {
            "store_conversations": self._store_conversations,
            "retention_days": self._retention_days,
        }

    async def set_app_settings(self, store_conversations=None, retention_days=None) -> dict:
        if store_conversations is not None:
            self._store_conversations = store_conversations
        if retention_days is not None:
            self._retention_days = retention_days
        return await self.app_settings()

    async def start_conversation(self, device_id: str, surface: str, role_name: str) -> int:
        cid = len(self.started) + 1
        self.started.append(cid)
        return cid

    async def record_turn(self, conversation_id, *, question, reply, emotion,
                          prompt_tokens, completion_tokens, latency_ms) -> None:
        self.turns.append((question, reply, emotion))

    async def end_conversation(self, conversation_id: int) -> None:
        self.ended.append(conversation_id)

    async def purge_expired(self) -> int:
        return 0

    async def recent_facts(self, device_id: str, limit: int = 20) -> list[str]:
        return list(self.facts.get(device_id, []))

    async def relevant_facts(
        self, device_id: str, query_embedding, limit: int = 6
    ) -> list[tuple[str, float]]:
        # No ranking here: the fake ignores the query and returns whatever
        # was seeded, with a fixed, descending score - the fake's job is the
        # shape, not ranking.
        if query_embedding is None:
            return []
        return [
            (f, 1.0 - i / 100)
            for i, f in enumerate(list(self.facts.get(device_id, []))[:limit])
        ]

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

    async def delete_conversations(self, device_id: str) -> None:
        self.deleted_conversations_for.append(device_id)

    async def forget(self, device_id: str) -> None:
        self.facts[device_id] = []
        self.standing[device_id] = []
        self._memory[device_id] = []
        await self.delete_conversations(device_id)

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

class FakeWebSocket:
    """A device's socket, without a device or a socket.

    Exists because Starlette's TestClient cannot reliably serve an HTTP
    request while one of its websocket sessions is open - the two contend on
    its single blocking portal, and the request either loses a pooled
    database connection or hangs outright. That is a limitation of the test
    client, not of the app: uvicorn serves both on one event loop without
    difficulty. So the firmware tests register one of these in the device
    registry instead of opening a real session, and the whole push runs as
    an ordinary HTTP request with nothing to contend with.

    The real integration proof lives outside pytest: scripts/fake_device.py
    against a running server.
    """

    def __init__(self) -> None:
        self.texts: list[str] = []
        self.blob = bytearray()
        self.order: list[str] = []

    async def send_text(self, data: str) -> None:
        self.texts.append(data)
        self.order.append("text")

    async def send_bytes(self, data: bytes) -> None:
        self.blob += data
        self.order.append(f"bytes:{len(data)}")

    @property
    def frames(self) -> list[dict]:
        import json

        return [json.loads(text) for text in self.texts]

    @property
    def binary_sizes(self) -> list[int]:
        return [int(item.split(":", 1)[1]) for item in self.order if item.startswith("bytes:")]


class BrokenWebSocket(FakeWebSocket):
    """Dies the moment it is handed the first byte of an image."""

    async def send_bytes(self, data: bytes) -> None:
        raise ConnectionResetError("the device went away")

class PacingWebSocket(FakeWebSocket):
    """A device that acks like the firmware does, so a paced push completes.

    The real one acks every OL_ACK_EVERY bytes from inside its websocket
    client's own task. Here the ack is pushed straight onto the link's queue
    the moment the threshold is crossed, which is the same contract without
    the flash writes.
    """

    def __init__(self, every: int) -> None:
        super().__init__()
        self.every = every
        self.link = None  # set by the test once the link exists
        self._acked = 0

    async def send_bytes(self, data: bytes) -> None:
        await super().send_bytes(data)
        if self.link is not None and len(self.blob) - self._acked >= self.every:
            self._acked = len(self.blob)
            self.link.acks.put_nowait({"type": "ota_ack", "have": self._acked})


class SilentWebSocket(FakeWebSocket):
    """Takes bytes and never acks - a device wedged in a flash write."""
