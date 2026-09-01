import json
from contextlib import contextmanager

import pytest
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from server.config import Settings
from server.main import create_app
from tests.fakes import FakeAccounts, FakeEmbedder, FakeLLM, FakeSTT, FakeStore, FakeTTS, SlowTTS


@contextmanager
def client(stt=None, tts=None, store=None, accounts=None, roles=None, **kw):
    kw.setdefault("session_cookie_secure", False)
    import asyncio
    import tempfile
    from server.roles import Roles

    tmp = tempfile.TemporaryDirectory()
    real_roles = roles
    # Only a Roles this helper built itself is this helper's to close. It is
    # always passed in already-constructed (create_app's `roles` argument),
    # so create_app's own lifespan sees roles_injected=True and skips closing
    # it - if this didn't, every one of client()'s uses would leak an
    # aiosqlite connection and its thread.
    owns_roles = real_roles is None
    if owns_roles:
        real_roles = Roles(f"sqlite+aiosqlite:///{tmp.name}/roles.db")
    app = create_app(
        settings=Settings(_env_file=None, **kw),
        stt=stt or FakeSTT(),
        llm=FakeLLM("Все добре."),
        tts=tts or FakeTTS(),
        store=store or FakeStore(),
        embedder=FakeEmbedder(),
        accounts=accounts or FakeAccounts(),
        roles=real_roles,
    )
    try:
        with TestClient(app) as c:
            yield c
    finally:
        if owns_roles:
            asyncio.run(real_roles.close())
        tmp.cleanup()


def test_client_closes_the_roles_engine_it_creates(monkeypatch):
    """client() builds a real Roles for every test and injects it, so the
    app's own lifespan shutdown skips closing it (roles_injected
    short-circuits). Left unclosed, each of the 40+ uses of client() leaks
    an aiosqlite connection and its thread."""
    from server.roles import Roles

    original_close = Roles.close
    closed = []

    async def spy_close(self):
        closed.append(True)
        await original_close(self)

    monkeypatch.setattr(Roles, "close", spy_close)

    with client() as c:
        _login(c)

    assert closed == [True]


def test_client_closes_the_roles_engine_even_when_the_test_raises(monkeypatch):
    from server.roles import Roles

    original_close = Roles.close
    closed = []

    async def spy_close(self):
        closed.append(True)
        await original_close(self)

    monkeypatch.setattr(Roles, "close", spy_close)

    with pytest.raises(RuntimeError):
        with client() as c:
            _login(c)
            raise RuntimeError("boom")

    assert closed == [True]


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


def test_me_requires_a_login():
    with client() as c:
        assert c.get("/me").status_code == 401


def test_me_names_the_logged_in_user():
    with client() as c:
        _login(c)
        assert c.get("/me").json()["username"] == "test"


def test_an_unknown_api_path_stays_a_json_404():
    """The SPA fallback must not turn a mistyped API call into an HTML page:
    a fetch() would parse it as JSON and fail somewhere far away."""
    with client() as c:
        r = c.get("/roles/nope/deeper")
    assert r.status_code == 404
    assert "text/html" not in r.headers.get("content-type", "")


def test_a_large_response_is_compressed(tmp_path):
    """Starlette's FileResponse and StaticFiles never compress on their own.

    Without middleware the face frames went over the wire as 1.2 MB of
    base64 - which voided the reason base64 was kept ("gzipped they are
    42 KB"). That was only true if something actually gzipped them.
    """
    (tmp_path / "assets").mkdir()
    (tmp_path / "index.html").write_text("<!doctype html><title>app</title>")
    (tmp_path / "big.json").write_text('{"padding":"' + "x" * 4000 + '"}')
    with client(web_dist=str(tmp_path)) as c:
        r = c.get("/big.json", headers={"Accept-Encoding": "gzip"})
    assert r.status_code == 200
    assert r.headers.get("content-encoding") == "gzip"


def test_a_small_response_is_not_compressed():
    """Below the threshold, compression costs more than it saves."""
    with client() as c:
        r = c.get("/healthz", headers={"Accept-Encoding": "gzip"})
    assert r.status_code == 200
    assert "content-encoding" not in r.headers


def test_a_missing_bundle_does_not_break_the_api():
    """web/dist is git-ignored, so a fresh clone has no bundle. The API must
    still answer rather than failing at import.

    Points web_dist at a directory that does not exist: the previous version
    of this test asserted /healthz against whatever bundle happened to be on
    disk, so it passed without ever reaching the branch it is named for.
    """
    with client(web_dist="/nonexistent/bundle/path") as c:
        assert c.get("/healthz").status_code == 200
        # No bundle, so navigation has nothing to serve and must not 500.
        assert c.get("/").status_code == 404


def test_a_file_at_the_root_of_the_bundle_is_served_as_itself(tmp_path):
    """Vite puts favicons and manifests at the bundle root, not under
    assets/, so the catch-all used to answer them with index.html - a
    favicon request got a 200 of HTML."""
    (tmp_path / "assets").mkdir()
    (tmp_path / "index.html").write_text("<!doctype html><title>app</title>")
    (tmp_path / "favicon.svg").write_text("<svg xmlns='http://www.w3.org/2000/svg'/>")
    with client(web_dist=str(tmp_path)) as c:
        icon = c.get("/favicon.svg")
        assert icon.status_code == 200
        assert "svg" in icon.headers["content-type"]
        # A navigation path still gets the app.
        assert "<title>app</title>" in c.get("/some/deep/view").text


def test_the_bundle_cannot_serve_a_file_outside_itself(tmp_path):
    """The path comes from the URL, so it gets resolved and checked."""
    (tmp_path / "assets").mkdir()
    (tmp_path / "index.html").write_text("<!doctype html><title>app</title>")
    secret = tmp_path.parent / "secret.txt"
    secret.write_text("not yours")
    with client(web_dist=str(tmp_path)) as c:
        r = c.get("/../secret.txt")
    assert "not yours" not in r.text


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


# ---------------------------------------------------------------- roles API

def _login(c):
    r = c.post("/login", json={"username": "test", "password": "test123"})
    assert r.status_code == 200
    return c


def test_roles_require_login():
    with client() as c:
        assert c.get("/roles").status_code == 401
        assert c.post("/roles", json={"name": "x"}).status_code == 401
        assert c.put("/roles/1", json={"max_sentences": 2}).status_code == 401
        assert c.delete("/roles/1").status_code == 401
        assert c.get("/settings/surfaces").status_code == 401
        assert c.put("/settings/surfaces/web", json={"role_id": 1}).status_code == 401


def test_roles_lists_the_two_defaults_once_logged_in():
    with client() as c:
        names = [r["name"] for r in _login(c).get("/roles").json()]
    assert sorted(names) == ["Device default", "Web default"]


def test_creating_a_role_then_listing_it():
    with client() as c:
        _login(c)
        created = c.post("/roles", json={
            "name": "Coach", "prompt": "You are a blunt coach.", "max_sentences": 3,
            "markdown_allowed": False, "languages": ["uk", "en"], "pinned_mood": "excited",
        })
        assert created.status_code == 200
        role = next(r for r in c.get("/roles").json() if r["name"] == "Coach")
    assert role["id"] == created.json()["id"]
    assert role["languages"] == ["uk", "en"]
    assert role["pinned_mood"] == "excited"


def test_a_duplicate_role_name_is_a_conflict():
    with client() as c:
        _login(c)
        body = {"name": "Coach", "prompt": None, "max_sentences": 2,
                "markdown_allowed": False, "languages": ["uk"], "pinned_mood": None}
        assert c.post("/roles", json=body).status_code == 200
        assert c.post("/roles", json=body).status_code == 409


def test_a_language_with_no_voice_is_rejected():
    with client() as c:
        _login(c)
        r = c.post("/roles", json={"name": "DE", "prompt": None, "max_sentences": 2,
                                   "markdown_allowed": False, "languages": ["de"],
                                   "pinned_mood": None})
    assert r.status_code == 422


def test_a_prompt_over_2000_characters_is_rejected_on_create():
    """RoleIn.prompt has no length limit, and build_messages trims history
    but never the system prompt - a pasted essay is sent in full on every
    device turn, against the 6000-tokens-per-minute free-tier ceiling
    MAX_CONTEXT_TOKENS exists to respect. An over-long prompt should be a
    422 at the edge, not a budget failure at the provider."""
    with client() as c:
        _login(c)
        r = c.post("/roles", json={
            "name": "Essay", "prompt": "x" * 2001, "max_sentences": 2,
            "markdown_allowed": False, "languages": ["uk"], "pinned_mood": None,
        })
    assert r.status_code == 422


def test_a_prompt_of_exactly_2000_characters_is_accepted_on_create():
    with client() as c:
        _login(c)
        r = c.post("/roles", json={
            "name": "Essay", "prompt": "x" * 2000, "max_sentences": 2,
            "markdown_allowed": False, "languages": ["uk"], "pinned_mood": None,
        })
    assert r.status_code == 200


def test_a_prompt_over_2000_characters_is_rejected_on_update():
    with client() as c:
        _login(c)
        role_id = c.post("/roles", json={"name": "Coach", "prompt": None, "max_sentences": 2,
                                         "markdown_allowed": False, "languages": ["uk"],
                                         "pinned_mood": None}).json()["id"]
        r = c.put(f"/roles/{role_id}", json={"prompt": "x" * 2001})
    assert r.status_code == 422


def test_a_mood_that_is_not_a_face_is_rejected():
    with client() as c:
        _login(c)
        r = c.post("/roles", json={"name": "Smug", "prompt": None, "max_sentences": 2,
                                   "markdown_allowed": False, "languages": ["uk"],
                                   "pinned_mood": "smug"})
    assert r.status_code == 422


def test_updating_a_role_changes_only_what_was_sent():
    with client() as c:
        _login(c)
        role_id = c.post("/roles", json={"name": "Coach", "prompt": "Blunt.",
                                         "max_sentences": 3, "markdown_allowed": False,
                                         "languages": ["uk"], "pinned_mood": None}).json()["id"]
        assert c.put(f"/roles/{role_id}", json={"max_sentences": 5}).status_code == 200
        role = next(r for r in c.get("/roles").json() if r["id"] == role_id)
    assert role["max_sentences"] == 5 and role["prompt"] == "Blunt."


def test_clearing_a_prompt_reverts_to_the_built_in_wording():
    """prompt: null is a real value, not an omission - it is the revert."""
    with client() as c:
        _login(c)
        device = next(r for r in c.get("/roles").json() if r["name"] == "Device default")
        c.put(f"/roles/{device['id']}", json={"prompt": "You are a pirate."})
        assert next(r for r in c.get("/roles").json()
                    if r["id"] == device["id"])["prompt"] == "You are a pirate."
        c.put(f"/roles/{device['id']}", json={"prompt": None})
        assert next(r for r in c.get("/roles").json()
                    if r["id"] == device["id"])["prompt"] is None


def test_updating_an_unknown_role_is_404():
    with client() as c:
        assert _login(c).put("/roles/999", json={"max_sentences": 2}).status_code == 404


def test_an_explicit_null_language_list_is_a_422_not_a_500():
    """Regression: Roles.update used to reach ",".join(None) for an explicit
    languages: null, raising an unhandled TypeError (500) instead of the
    ValueError the endpoint turns into a 422."""
    with client() as c:
        _login(c)
        role_id = c.post("/roles", json={"name": "Coach", "prompt": None, "max_sentences": 2,
                                         "markdown_allowed": False, "languages": ["uk"],
                                         "pinned_mood": None}).json()["id"]
        r = c.put(f"/roles/{role_id}", json={"languages": None})
    assert r.status_code == 422


def test_an_explicit_null_name_is_a_422_not_a_500():
    """name is NOT NULL; an explicit null used to reach the column and raise
    an IntegrityError (500) instead of the ValueError the endpoint turns
    into a 422 - the same shape of bug languages=None had."""
    with client() as c:
        _login(c)
        role_id = c.post("/roles", json={"name": "Coach", "prompt": None, "max_sentences": 2,
                                         "markdown_allowed": False, "languages": ["uk"],
                                         "pinned_mood": None}).json()["id"]
        r = c.put(f"/roles/{role_id}", json={"name": None})
    assert r.status_code == 422


def test_an_explicit_null_max_sentences_is_a_422_not_a_500():
    with client() as c:
        _login(c)
        role_id = c.post("/roles", json={"name": "Coach", "prompt": None, "max_sentences": 2,
                                         "markdown_allowed": False, "languages": ["uk"],
                                         "pinned_mood": None}).json()["id"]
        r = c.put(f"/roles/{role_id}", json={"max_sentences": None})
    assert r.status_code == 422


def test_an_explicit_null_markdown_allowed_is_a_422_not_a_500():
    with client() as c:
        _login(c)
        role_id = c.post("/roles", json={"name": "Coach", "prompt": None, "max_sentences": 2,
                                         "markdown_allowed": False, "languages": ["uk"],
                                         "pinned_mood": None}).json()["id"]
        r = c.put(f"/roles/{role_id}", json={"markdown_allowed": None})
    assert r.status_code == 422


def test_a_language_named_taken_is_still_a_422_not_a_409():
    """_validate interpolates the client's own value into its message, so a
    naive `"taken" in str(exc)` check for the 409/422 split would mistake an
    invalid language for a duplicate-name conflict."""
    with client() as c:
        _login(c)
        r = c.post("/roles", json={"name": "X", "prompt": None, "max_sentences": 2,
                                   "markdown_allowed": False, "languages": ["taken"],
                                   "pinned_mood": None})
    assert r.status_code == 422


def test_a_mood_named_taken_is_still_a_422_not_a_409():
    with client() as c:
        _login(c)
        r = c.post("/roles", json={"name": "Y", "prompt": None, "max_sentences": 2,
                                   "markdown_allowed": False, "languages": ["uk"],
                                   "pinned_mood": "taken"})
    assert r.status_code == 422


def test_updating_a_role_to_a_taken_name_is_a_conflict():
    with client() as c:
        _login(c)
        c.post("/roles", json={"name": "Coach", "prompt": None, "max_sentences": 2,
                               "markdown_allowed": False, "languages": ["uk"], "pinned_mood": None})
        other_id = c.post("/roles", json={"name": "Other", "prompt": None, "max_sentences": 2,
                                          "markdown_allowed": False, "languages": ["uk"],
                                          "pinned_mood": None}).json()["id"]
        r = c.put(f"/roles/{other_id}", json={"name": "Coach"})
    assert r.status_code == 409


def test_setting_an_unknown_surfaces_role_is_404():
    """/settings/surfaces/{surface} used to accept any string and write a
    junk row into surface_roles that /settings/surfaces never displays."""
    with client() as c:
        _login(c)
        role_id = c.post("/roles", json={"name": "X", "prompt": None, "max_sentences": 2,
                                         "markdown_allowed": False, "languages": ["uk"],
                                         "pinned_mood": None}).json()["id"]
        r = c.put("/settings/surfaces/bogus", json={"role_id": role_id})
    assert r.status_code == 404


def test_the_esp32_surfaces_active_role_reaches_the_llm():
    """Task 4's central wiring: ws_endpoint reads
    app.state.roles.active_for(surface) into Session(role=...), and that
    role's prompt has to be what the LLM actually receives. A previous test
    that was the only end-to-end guard of this seam was deleted without a
    replacement, and three more tasks build on it."""
    with client(device_token="s3cret") as c:
        _login(c)
        role_id = c.post("/roles", json={
            "name": "Pirate",
            "prompt": "Ye speak only as Captain Sparrow the parrot, arr.",
            "max_sentences": 2, "markdown_allowed": False,
            "languages": ["uk", "ru", "en"], "pinned_mood": None,
        }).json()["id"]
        assert c.put("/settings/surfaces/esp32", json={"role_id": role_id}).status_code == 200

        with c.websocket_connect("/ws", headers={"Authorization": "Bearer s3cret"}) as ws:
            ws.send_text(json.dumps({"type": "start"}))
            assert json.loads(ws.receive_text())["value"] == "listening"
            ws.send_bytes(b"\x00\x01" * 32000)
            ws.send_text(json.dumps({"type": "end"}))
            controls(ws, answered())

        prompt = c.app.state.llm.prompts[0][0]["content"]
    assert prompt.startswith("Ye speak only as Captain Sparrow the parrot, arr.")


def test_the_esp32_surfaces_default_role_sends_the_measured_base_prompt():
    """The other half of the same seam: with no custom role in play, the
    prompt the LLM receives for the device surface must be byte-identical to
    server.persona.BASE."""
    from server.persona import BASE

    with client(device_token="s3cret") as c:
        with c.websocket_connect("/ws", headers={"Authorization": "Bearer s3cret"}) as ws:
            ws.send_text(json.dumps({"type": "start"}))
            assert json.loads(ws.receive_text())["value"] == "listening"
            ws.send_bytes(b"\x00\x01" * 32000)
            ws.send_text(json.dumps({"type": "end"}))
            controls(ws, answered())

        prompt = c.app.state.llm.prompts[0][0]["content"]
    assert prompt == BASE


def test_a_built_in_role_cannot_be_deleted():
    with client() as c:
        _login(c)
        device = next(r for r in c.get("/roles").json() if r["name"] == "Device default")
        assert c.delete(f"/roles/{device['id']}").status_code == 409


def test_built_in_flag_appears_in_the_roles_list():
    with client() as c:
        by_name = {r["name"]: r["built_in"] for r in _login(c).get("/roles").json()}
    assert by_name == {"Device default": True, "Web default": True}


def test_a_created_role_is_not_built_in():
    with client() as c:
        _login(c)
        role_id = c.post("/roles", json={"name": "Coach", "prompt": None, "max_sentences": 2,
                                         "markdown_allowed": False, "languages": ["uk"],
                                         "pinned_mood": None}).json()["id"]
        role = next(r for r in c.get("/roles").json() if r["id"] == role_id)
    assert role["built_in"] is False


def test_renaming_a_built_in_role_via_the_api_is_refused_and_delete_stays_refused():
    """The reviewer's exact repro: update(name=...) then delete() used to
    turn the 409 into a 200 and a built-in role would be gone."""
    with client() as c:
        _login(c)
        device = next(r for r in c.get("/roles").json() if r["name"] == "Device default")
        r = c.put(f"/roles/{device['id']}", json={"name": "Foo"})
        assert r.status_code == 422
        assert c.delete(f"/roles/{device['id']}").status_code == 409


def test_switching_the_active_role_for_a_surface():
    with client() as c:
        _login(c)
        role_id = c.post("/roles", json={"name": "Terse", "prompt": None, "max_sentences": 1,
                                         "markdown_allowed": False, "languages": ["uk"],
                                         "pinned_mood": None}).json()["id"]
        assert c.put("/settings/surfaces/esp32", json={"role_id": role_id}).status_code == 200
        surfaces = c.get("/settings/surfaces").json()
    assert surfaces["esp32"]["name"] == "Terse"
    assert surfaces["web"]["name"] == "Web default"


def test_switching_to_an_unknown_role_is_404():
    with client() as c:
        assert _login(c).put("/settings/surfaces/web",
                             json={"role_id": 999}).status_code == 404


def test_deleting_the_active_role_returns_the_surface_to_its_default():
    with client() as c:
        _login(c)
        role_id = c.post("/roles", json={"name": "Terse", "prompt": None, "max_sentences": 1,
                                         "markdown_allowed": False, "languages": ["uk"],
                                         "pinned_mood": None}).json()["id"]
        c.put("/settings/surfaces/esp32", json={"role_id": role_id})
        assert c.delete(f"/roles/{role_id}").status_code == 200
        surfaces = c.get("/settings/surfaces").json()
    assert surfaces["esp32"]["name"] == "Device default"


def test_switching_the_active_role_changes_the_next_turns_prompt_without_reconnecting():
    """The ESP32 holds one long-lived socket; role= used to be frozen at the
    handshake, so switching the active role did nothing until the device
    reconnected. It must now reach the very next question on an already-open
    connection."""
    with client(device_token="s3cret") as c:
        _login(c)
        role_a = c.post("/roles", json={
            "name": "Role A", "prompt": "You are Role A.", "max_sentences": 2,
            "markdown_allowed": False, "languages": ["uk", "ru", "en"], "pinned_mood": None,
        }).json()["id"]
        role_b = c.post("/roles", json={
            "name": "Role B", "prompt": "You are Role B.", "max_sentences": 2,
            "markdown_allowed": False, "languages": ["uk", "ru", "en"], "pinned_mood": None,
        }).json()["id"]
        assert c.put("/settings/surfaces/esp32", json={"role_id": role_a}).status_code == 200

        with c.websocket_connect("/ws", headers={"Authorization": "Bearer s3cret"}) as ws:
            ws.send_text(json.dumps({"type": "start"}))
            assert json.loads(ws.receive_text())["value"] == "listening"
            ws.send_bytes(b"\x00\x01" * 32000)
            ws.send_text(json.dumps({"type": "end"}))
            controls(ws, answered())

            assert c.put("/settings/surfaces/esp32", json={"role_id": role_b}).status_code == 200

            ws.send_text(json.dumps({"type": "start"}))
            assert json.loads(ws.receive_text())["value"] == "listening"
            ws.send_bytes(b"\x00\x01" * 32000)
            ws.send_text(json.dumps({"type": "end"}))
            controls(ws, answered())

        prompts = c.app.state.llm.prompts
    assert prompts[0][0]["content"].startswith("You are Role A.")
    assert prompts[1][0]["content"].startswith("You are Role B.")


def test_a_bearer_socket_never_receives_a_trace_frame():
    with client(device_token="s3cret") as c:
        with c.websocket_connect("/ws", headers={"Authorization": "Bearer s3cret"}) as ws:
            ws.send_text(json.dumps({"type": "text", "value": "Як справи?"}))
            seen = []
            for _ in range(12):
                frame = json.loads(ws.receive_text())
                seen.append(frame["type"])
                if frame["type"] == "state" and frame["value"] == "idle":
                    break
    assert "trace" not in seen


def test_a_cookie_authorised_socket_does_receive_a_trace_frame():
    """The bearer-token negative case above was covered over the wire, but
    the positive case - a logged-in browser actually getting the debugging
    frame it needs - was only ever exercised at the Session level
    (test_session.py's test_a_web_session_gets_a_trace_frame), never through
    a real WebSocket."""
    with client(device_token="s3cret") as c:
        _login(c)
        with c.websocket_connect("/ws") as ws:
            ws.send_text(json.dumps({"type": "text", "value": "Як справи?"}))
            seen = []
            for _ in range(12):
                frame = json.loads(ws.receive_text())
                seen.append(frame["type"])
                if frame["type"] == "state" and frame["value"] == "idle":
                    break
    assert "trace" in seen


# --------------------------------------------------- conversations API

def test_deleting_conversations_requires_login():
    with client() as c:
        assert c.delete("/conversations/default").status_code == 401


def test_deleting_conversations_deletes_and_leaves_facts_intact():
    from tests.fakes import FakeStore

    store = FakeStore()
    with client(store=store) as c:
        _login(c)
        c.post("/memory/default", json={"text": "keep me"})
        r = c.delete("/conversations/default")
        assert r.status_code == 200 and r.json() == {"status": "ok"}
        listed = c.get("/memory/default").json()
    assert any(item["text"] == "keep me" for item in listed)
    assert store.deleted_conversations_for == ["default"]


def test_lifespan_purges_expired_rows_on_startup():
    """Deleting the purge_expired() call from the lifespan left every test
    passing, on what is now one of two retention paths (the other runs at
    the end of each session - see Session.finish). client() always injects a
    real Store, and an injected store's lifespan skips backfill/purge
    entirely, so this has to build the app the way production does: with no
    store at all, so create_app constructs its own real Store and the
    startup path actually runs."""
    import tempfile

    from server.memory.store import Store

    called = []
    original = Store.purge_expired

    async def spy(self):
        called.append(True)
        return await original(self)

    import unittest.mock

    with unittest.mock.patch.object(Store, "purge_expired", spy):
        with tempfile.TemporaryDirectory() as tmp:
            app = create_app(
                settings=Settings(
                    _env_file=None, session_cookie_secure=False, db_path=f"{tmp}/test.db",
                ),
                stt=FakeSTT(), llm=FakeLLM(), tts=FakeTTS(),
                embedder=FakeEmbedder(), accounts=FakeAccounts(),
            )
            with TestClient(app):
                pass

    assert called == [True]


def test_app_settings_require_login():
    with client() as c:
        assert c.get("/settings/app").status_code == 401
        assert c.put("/settings/app", json={"retention_days": 7}).status_code == 401


def test_app_settings_round_trip_over_http():
    with client() as c:
        _login(c)
        assert c.get("/settings/app").json() == {
            "store_conversations": True, "retention_days": 90,
        }
        updated = c.put("/settings/app", json={"store_conversations": False}).json()
    assert updated["store_conversations"] is False


def test_put_settings_app_retention_days_reaches_the_store():
    """FakeStore.set_app_settings used to drop its retention_days argument,
    so nothing exercised PUT /settings/app {"retention_days": N} actually
    reaching the store."""
    with client() as c:
        _login(c)
        updated = c.put("/settings/app", json={"retention_days": 7}).json()
        again = c.get("/settings/app").json()
    assert updated["retention_days"] == 7
    assert again["retention_days"] == 7
