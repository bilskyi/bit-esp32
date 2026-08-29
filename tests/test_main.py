import json
from contextlib import contextmanager

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from server.config import Settings
from server.main import create_app
from tests.fakes import FakeAccounts, FakeEmbedder, FakeLLM, FakeSTT, FakeStore, FakeTTS


@contextmanager
def client(store=None, accounts=None, **kw):
    app = create_app(
        settings=Settings(_env_file=None, **kw),
        stt=FakeSTT(),
        llm=FakeLLM("Все добре."),
        tts=FakeTTS(),
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
