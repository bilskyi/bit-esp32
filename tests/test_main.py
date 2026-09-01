import json
from contextlib import contextmanager

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from server.config import Settings
from server.main import create_app
from server.persona import BASE
from tests.fakes import FakeAccounts, FakeEmbedder, FakeLLM, FakeSTT, FakeStore, FakeTTS, SlowTTS


@contextmanager
def client(stt=None, tts=None, store=None, accounts=None, **kw):
    kw.setdefault("session_cookie_secure", False)
    app = create_app(
        settings=Settings(_env_file=None, **kw),
        stt=stt or FakeSTT(),
        llm=FakeLLM("Все добре."),
        tts=tts or FakeTTS(),
        store=store or FakeStore(),
        embedder=FakeEmbedder(),
        accounts=accounts or FakeAccounts(),
    )
    with TestClient(app) as c:
        yield c


def test_healthz_reports_ok():
    with client() as c:
        r = c.get("/healthz")
    assert r.status_code == 200 and r.json()["status"] == "ok"


def test_websocket_is_open_when_no_device_token_is_configured():
    with client() as c, c.websocket_connect("/ws") as ws:
        ws.send_text(json.dumps({"type": "start"}))
        assert json.loads(ws.receive_text())["value"] == "listening"


def test_websocket_rejects_a_missing_token_when_auth_is_on():
    with client(device_token="s3cret") as c, pytest.raises(WebSocketDisconnect):
        with c.websocket_connect("/ws") as ws:
            ws.receive_text()


def test_websocket_rejects_a_wrong_token():
    with client(device_token="s3cret") as c, pytest.raises(WebSocketDisconnect):
        with c.websocket_connect("/ws", headers={"Authorization": "Bearer wrong"}) as ws:
            ws.receive_text()


def test_websocket_accepts_the_right_token():
    with client(device_token="s3cret") as c, c.websocket_connect(
        "/ws", headers={"Authorization": "Bearer s3cret"}
    ) as ws:
        ws.send_text(json.dumps({"type": "start"}))
        assert json.loads(ws.receive_text())["value"] == "listening"


def test_a_whole_turn_travels_over_the_wire():
    with client() as c, c.websocket_connect("/ws") as ws:
        ws.send_text(json.dumps({"type": "start"}))
        assert json.loads(ws.receive_text())["value"] == "listening"
        ws.send_bytes(b"\x00\x01" * 32000)  # 2 s: under a second is refused as too short to be speech
        ws.send_text(json.dumps({"type": "end"}))

        states, audio_frames, done = [], 0, False
        for _ in range(20):
            message = ws.receive()
            if "bytes" in message and message["bytes"] is not None:
                audio_frames += 1
                continue
            payload = json.loads(message["text"])
            if payload["type"] == "state":
                states.append(payload["value"])
            elif payload["type"] == "done":
                done = True
            if states and states[-1] == "idle":
                break
        assert states == ["thinking", "speaking", "idle"]
        assert audio_frames > 0
        assert done


def test_unknown_control_messages_are_ignored():
    with client() as c, c.websocket_connect("/ws") as ws:
        ws.send_text(json.dumps({"type": "nonsense"}))
        ws.send_text(json.dumps({"type": "start"}))
        assert json.loads(ws.receive_text())["value"] == "listening"


def test_malformed_json_does_not_kill_the_connection():
    with client() as c, c.websocket_connect("/ws") as ws:
        ws.send_text("{not json")
        ws.send_text(json.dumps({"type": "start"}))
        assert json.loads(ws.receive_text())["value"] == "listening"


def controls(ws, until, limit=400):
    """Read frames, discarding audio, until `until` matches a control frame."""
    seen = []
    for _ in range(limit):
        message = ws.receive()
        if message.get("bytes") is not None:
            continue
        if message.get("text") is None:
            continue
        payload = json.loads(message["text"])
        seen.append(payload)
        if until(payload):
            return seen
    raise AssertionError(f"never saw the frame we were waiting for: {seen}")


def answered():
    """Matches the "idle" that closes a turn, not one sent before it began."""
    thinking = False

    def match(payload):
        nonlocal thinking
        thinking = thinking or payload.get("value") == "thinking"
        return thinking and payload.get("value") == "idle"

    return match


def test_an_utterance_the_device_abandons_does_not_deafen_the_session():
    """The 28 Aug stall, over the wire.

    The device interrupts a reply and immediately starts the next question -
    cancel, then start forty milliseconds later. The "done" closing the
    cancelled reply then lands while the mic is already recording, and until
    this was fixed in voice_main.c the device's playback task forced it back to
    idle: the question was recorded into a state that never sends "end", so the
    release sent nothing and the next thing the device did was press again.

    From the server's log that day: a start accepted 0.19 s after a cancel,
    0.22 s of audio buffered, no "end", and sixty seconds later "utterance
    timed out after 60.0s". The user heard nothing for the whole minute and
    eventually asked the device "почему-то только что не отвечал".

    The device is fixed, but it must not be the only thing standing between a
    dropped "end" and a session that has stopped answering.
    """
    stt = FakeSTT()
    with client(stt=stt, tts=SlowTTS(chunks=200, delay=0.01)) as c:
        with c.websocket_connect("/ws") as ws:
            ws.send_text(json.dumps({"type": "start"}))
            ws.send_bytes(b"\x00\x01" * 32000)
            ws.send_text(json.dumps({"type": "end"}))
            controls(ws, lambda p: p.get("value") == "speaking")

            # The press: silence the reply, then start the next question.
            ws.send_text(json.dumps({"type": "cancel"}))
            ws.send_text(json.dumps({"type": "start"}))
            ws.send_bytes(b"\x7f\x7f" * 3520)  # the 0.22 s that got recorded

            # ...and here the device went deaf. No "end" is ever sent for it.
            # The user presses again and asks the question a second time.
            ws.send_text(json.dumps({"type": "start"}))
            ws.send_bytes(b"\x00\x01" * 32000)
            ws.send_text(json.dumps({"type": "end"}))

            # Not simply "the next idle": the cancelled reply sends one of its
            # own on the way out, ahead of anything the new question causes.
            seen = controls(ws, answered())

    states = [p["value"] for p in seen if p.get("type") == "state"]
    assert states[-3:] == ["thinking", "speaking", "idle"], (
        "the second question was never answered"
    )
    assert stt.received[-1] == 2 * 32000, (
        "the abandoned fragment was prepended to the question"
    )


def test_text_frame_gets_a_written_reply_with_no_audio():
    with client() as c, c.websocket_connect("/ws") as ws:
        ws.send_text(json.dumps({"type": "text", "value": "Привіт!"}))
        seen, audio_frames = [], 0
        for _ in range(20):
            message = ws.receive()
            if message.get("bytes") is not None:
                audio_frames += 1
                continue
            payload = json.loads(message["text"])
            seen.append(payload)
            if payload.get("value") == "idle":
                break
    assert audio_frames == 0
    assert any(p.get("type") == "reply" for p in seen)


# ---------------------------------------------------------- memory / customization API

def test_memory_list_is_open_when_no_token_is_configured():
    with client() as c:
        r = c.get("/memory/default")
    assert r.status_code == 200 and r.json() == []


def test_memory_list_requires_auth_when_configured():
    with client(device_token="s3cret") as c:
        r = c.get("/memory/default")
    assert r.status_code == 401


def test_memory_list_accepts_the_right_token():
    with client(device_token="s3cret") as c:
        r = c.get("/memory/default", headers={"Authorization": "Bearer s3cret"})
    assert r.status_code == 200


def test_posting_a_standing_instruction_makes_it_listable():
    with client() as c:
        added = c.post("/memory/default", json={"text": "Always answer informally"})
        assert added.status_code == 200
        listed = c.get("/memory/default").json()
    assert any(
        item["text"] == "Always answer informally" and item["source"] == "user"
        for item in listed
    )


def test_deleting_one_entry_leaves_the_others():
    with client() as c:
        keep = c.post("/memory/default", json={"text": "keep me"}).json()["id"]
        drop = c.post("/memory/default", json={"text": "drop me"}).json()["id"]
        c.delete(f"/memory/default/{drop}")
        listed = c.get("/memory/default").json()
    ids = {item["id"] for item in listed}
    assert keep in ids and drop not in ids


def test_deleting_an_unknown_entry_is_a_404():
    with client() as c:
        r = c.delete("/memory/default/999")
    assert r.status_code == 404


def test_clearing_a_device_removes_everything():
    with client() as c:
        c.post("/memory/default", json={"text": "temporary"})
        c.delete("/memory/default")
        listed = c.get("/memory/default").json()
    assert listed == []


def test_memory_is_scoped_per_device():
    with client() as c:
        c.post("/memory/dev1", json={"text": "dev1 only"})
        listed = c.get("/memory/dev2").json()
    assert listed == []


def test_memory_reachable_via_login_cookie_with_no_bearer_token():
    with client(device_token="s3cret") as c:
        c.post("/login", json={"username": "test", "password": "test123"})
        r = c.get("/memory/default")
    assert r.status_code == 200


def test_memory_still_reachable_via_bearer_token_with_no_login():
    with client(device_token="s3cret") as c:
        r = c.get("/memory/default", headers={"Authorization": "Bearer s3cret"})
    assert r.status_code == 200


def test_memory_rejects_neither_credential():
    with client(device_token="s3cret") as c:
        r = c.get("/memory/default")
    assert r.status_code == 401


def test_memory_ui_is_served_with_no_token_needed():
    """The page itself carries no data; only its own fetch calls are gated."""
    with client(device_token="s3cret") as c:
        r = c.get("/memory")
    assert r.status_code == 200
    assert r.headers["content-type"].startswith("text/html")
    assert "<html" in r.text.lower()


def test_memory_ui_never_builds_fact_text_as_markup():
    """XSS guard: the page must render fact text with textContent, not
    innerHTML - a stray fact should never become live markup in a browser."""
    with client() as c:
        r = c.get("/memory")
    assert "textContent = item.text" in r.text
    assert "innerHTML = item.text" not in r.text


def test_login_with_the_right_password_sets_a_session_cookie():
    with client() as c:
        r = c.post("/login", json={"username": "test", "password": "test123"})
        assert r.status_code == 200
        assert "session" in r.cookies


def test_login_with_the_wrong_password_is_rejected():
    with client() as c:
        r = c.post("/login", json={"username": "test", "password": "wrong"})
    assert r.status_code == 401


def test_login_with_an_unknown_username_is_rejected():
    with client() as c:
        r = c.post("/login", json={"username": "nobody", "password": "anything"})
    assert r.status_code == 401


def test_logout_clears_the_session():
    with client() as c:
        c.post("/login", json={"username": "test", "password": "test123"})
        r = c.post("/logout")
        assert r.status_code == 200
        assert "session" not in r.cookies or r.cookies.get("session") == ""


def test_websocket_accepts_a_valid_session_cookie_with_no_bearer_token():
    with client(device_token="s3cret") as c:
        c.post("/login", json={"username": "test", "password": "test123"})
        with c.websocket_connect("/ws") as ws:
            ws.send_text(json.dumps({"type": "start"}))
            assert json.loads(ws.receive_text())["value"] == "listening"


def test_websocket_still_rejects_a_bad_bearer_with_no_session():
    with client(device_token="s3cret") as c, pytest.raises(WebSocketDisconnect):
        with c.websocket_connect("/ws", headers={"Authorization": "Bearer wrong"}) as ws:
            ws.receive_text()


# -------------------------------------------------------- style settings API

def test_style_settings_requires_login():
    with client() as c:
        r = c.get("/settings/style/web")
    assert r.status_code == 401


def test_style_settings_roundtrips_after_login():
    with client() as c:
        c.post("/login", json={"username": "test", "password": "test123"})
        put = c.put("/settings/style/web", json={"max_sentences": 4, "markdown_allowed": True})
        assert put.status_code == 200
        got = c.get("/settings/style/web").json()
    assert got == {"surface": "web", "max_sentences": 4, "markdown_allowed": True}


def test_style_settings_default_before_any_override():
    with client() as c:
        c.post("/login", json={"username": "test", "password": "test123"})
        got = c.get("/settings/style/esp32").json()
    assert got == {"surface": "esp32", "max_sentences": 2, "markdown_allowed": False}


def test_a_web_style_override_does_not_affect_esp32():
    with client() as c:
        c.post("/login", json={"username": "test", "password": "test123"})
        c.put("/settings/style/web", json={"max_sentences": 4, "markdown_allowed": True})
        got = c.get("/settings/style/esp32").json()
    assert got == {"surface": "esp32", "max_sentences": 2, "markdown_allowed": False}


def test_esp32_style_cannot_be_overridden():
    with client() as c:
        c.post("/login", json={"username": "test", "password": "test123"})
        r = c.put("/settings/style/esp32", json={"max_sentences": 6, "markdown_allowed": True})
        assert r.status_code == 400
        got = c.get("/settings/style/esp32").json()
    assert got == {"surface": "esp32", "max_sentences": 2, "markdown_allowed": False}


def test_esp32_style_ignores_a_directly_seeded_override_row():
    """Guards the actual Critical bug: even if an esp32 override row exists
    in the database - however it got there, not just via the now-blocked
    API write - it must never reach the device's prompt."""
    store = FakeStore()
    store._styles["esp32"] = {"max_sentences": 6, "markdown_allowed": True}
    with client(store=store) as c:
        c.post("/login", json={"username": "test", "password": "test123"})
        got = c.get("/settings/style/esp32").json()
    assert got == {"surface": "esp32", "max_sentences": 2, "markdown_allowed": False}


def test_esp32_system_prompt_stays_base_despite_a_seeded_override():
    """Same guard as above, but checked against what a real Session actually
    sends the LLM, not just what the settings API reports."""
    store = FakeStore()
    store._styles["esp32"] = {"max_sentences": 6, "markdown_allowed": True}
    with client(store=store) as c, c.websocket_connect("/ws") as ws:
        ws.send_text(json.dumps({"type": "start"}))
        assert json.loads(ws.receive_text())["value"] == "listening"
        ws.send_bytes(b"\x00\x01" * 32000)
        ws.send_text(json.dumps({"type": "end"}))
        for _ in range(20):
            message = ws.receive()
            if "bytes" in message and message["bytes"] is not None:
                continue
            payload = json.loads(message["text"])
            if payload.get("type") == "state" and payload["value"] == "idle":
                break
        llm = c.app.state.llm
    assert llm.prompts[0][0] == {"role": "system", "content": BASE}
