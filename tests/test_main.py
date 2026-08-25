import json
from contextlib import contextmanager

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from server.config import Settings
from server.main import create_app
from tests.fakes import FakeLLM, FakeSTT, FakeStore, FakeTTS


@contextmanager
def client(**kw):
    app = create_app(
        settings=Settings(_env_file=None, **kw),
        stt=FakeSTT(),
        llm=FakeLLM("Все добре."),
        tts=FakeTTS(),
        store=FakeStore(),
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
        ws.send_bytes(b"\x00\x01" * 1600)
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
