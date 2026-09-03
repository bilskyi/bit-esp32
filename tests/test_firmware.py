"""The relay that carries a firmware image to the device.

No test here opens a real TestClient websocket and then makes an HTTP
request, and that constraint is the whole shape of this file.

Starlette's TestClient cannot reliably do both at once: its websocket
session and its HTTP transport contend on one blocking portal, and the
request either orphans a pooled database connection - surfacing later as
"the garbage collector is trying to clean up non-checked-in connection",
attributed to whatever unrelated test the collection happened to land in -
or hangs in TestClient.__exit__ outright. Isolated by elimination: a bare
client, a login, a websocket, and a websocket round trip are all clean; a
websocket plus one GET is what breaks.

That is a limitation of the test client, not of the app. uvicorn serves an
upload and a websocket on one event loop without difficulty, which is what
the end-to-end run against a real server proves (scripts/fake_device.py
--await-ota, and the transcript in the commit that added it).

So the device is a FakeWebSocket registered directly in the registry, the
push is an ordinary HTTP request, and the registry's own behaviour is
tested on the registry.
"""

import json
import struct

from tests.fakes import BrokenWebSocket, FakeWebSocket
from tests.test_main import client  # noqa: F401 - see the note below

# client() lives in test_main.py rather than conftest.py, and is imported
# rather than copied. Moving it to conftest would be the tidier home but
# touches forty-odd call sites in test_main.py for no behaviour change, and
# copying it here would leave two of them to drift apart.

from server.firmware import (
    CHIP_ID_ESP32C3,
    HEADER_MIN,
    OTA_CHUNK,
    DeviceLink,
    DeviceRegistry,
    check_image,
    image_version,
)

TOKEN = "s3cret"
DEVICE_HEADERS = {"Authorization": f"Bearer {TOKEN}"}


def firmware(size: int = OTA_CHUNK * 3 + 17, version: bytes = b"v0.2.1") -> bytes:
    """A byte string that passes check_image(), at an arbitrary size.

    The offsets are the ones ol_check_image() reads, and the C host test
    builds its header from the same four. Neither is a guess: they were read
    out of a real firmware/build/voice_capture.bin.
    """
    assert size >= HEADER_MIN
    head = bytearray(size)
    head[0x00] = 0xE9
    struct.pack_into("<H", head, 0x0C, CHIP_ID_ESP32C3)
    struct.pack_into("<I", head, 0x20, 0xABCD5432)
    head[0x30 : 0x30 + len(version) + 1] = version + b"\0"
    head[0x50 : 0x50 + 14] = b"voice_capture\0"
    # Something other than zeros in the body, so a test that reassembles the
    # bytes is actually comparing content.
    for i in range(0x90, size):
        head[i] = i % 251
    return bytes(head)


# --------------------------------------------------------------- the header


def test_check_image_accepts_a_real_header():
    assert check_image(firmware()) is None


def test_check_image_rejects_a_short_buffer():
    assert "short" in check_image(firmware()[: HEADER_MIN - 1])
    assert "short" in check_image(b"")


def test_check_image_rejects_a_zip():
    bad = bytearray(firmware())
    bad[0] = ord("P")
    assert "ESP" in check_image(bytes(bad))


def test_check_image_rejects_another_chip():
    bad = bytearray(firmware())
    struct.pack_into("<H", bad, 0x0C, 9)
    assert "chip" in check_image(bytes(bad))


def test_check_image_rejects_a_missing_app_descriptor():
    bad = bytearray(firmware())
    struct.pack_into("<I", bad, 0x20, 0)
    assert "descriptor" in check_image(bytes(bad))


def test_check_image_rejects_another_project():
    bad = bytearray(firmware())
    bad[0x50 : 0x50 + 32] = b"other_app\0".ljust(32, b"\0")
    assert "another project" in check_image(bytes(bad))


def test_check_image_rejects_our_name_as_a_prefix():
    bad = bytearray(firmware())
    bad[0x50 : 0x50 + 32] = b"voice_capture2\0".ljust(32, b"\0")
    assert "another project" in check_image(bytes(bad))


def test_image_version_is_read_out():
    assert image_version(firmware(version=b"v9.9.9")) == "v9.9.9"


def test_image_version_of_an_unterminated_field_is_none():
    bad = bytearray(firmware())
    bad[0x30 : 0x30 + 32] = b"x" * 32
    assert image_version(bytes(bad)) is None


# ----------------------------------------------------------------- the push


def connected(c, ws=None):
    """Put a stand-in device in the registry and hand back its socket."""
    fake = ws if ws is not None else FakeWebSocket()
    link = DeviceLink(fake)
    c.app.state.devices._links["esp32"] = link
    return fake, link


def login(c):
    r = c.post("/login", json={"username": "test", "password": "test123"})
    assert r.status_code == 200


def lines(response):
    return [json.loads(line) for line in response.text.splitlines() if line.strip()]


def test_push_requires_a_login():
    with client(device_token=TOKEN) as c:
        assert c.post("/firmware/push", content=firmware()).status_code == 401


def test_device_endpoint_requires_a_login():
    with client(device_token=TOKEN) as c:
        assert c.get("/firmware/device").status_code == 401


def test_push_with_no_device_connected_is_a_409():
    with client(device_token=TOKEN) as c:
        login(c)
        r = c.post("/firmware/push", content=firmware())
        assert r.status_code == 409
        assert "device" in r.text


def test_push_refuses_a_file_that_is_not_firmware():
    with client(device_token=TOKEN) as c:
        login(c)
        fake, _ = connected(c)
        bad = bytearray(firmware())
        bad[0] = ord("P")

        r = c.post("/firmware/push", content=bytes(bad))

        assert r.status_code == 400
        assert "ESP" in r.text
        # And the device was told nothing at all: a refused file must not cost
        # the running image its partition.
        assert fake.texts == []
        assert fake.blob == b""


def test_push_refuses_an_empty_body():
    with client(device_token=TOKEN) as c:
        login(c)
        fake, _ = connected(c)
        assert c.post("/firmware/push", content=b"").status_code == 400
        assert fake.texts == []


def test_push_refuses_something_far_too_large_to_be_firmware(monkeypatch):
    # The ceiling is lowered rather than the body raised: the constant is
    # what is under test, and four megabytes of test data is four megabytes
    # of nothing.
    monkeypatch.setattr("server.firmware.MAX_BODY", HEADER_MIN * 2)
    with client(device_token=TOKEN) as c:
        login(c)
        fake, _ = connected(c)
        # A valid header on the front, so size is the only thing wrong with
        # it - otherwise this would pass for the wrong reason.
        r = c.post("/firmware/push", content=firmware(size=HEADER_MIN * 2 + 1))
        assert r.status_code == 413
        assert "larger" in r.text
        assert fake.texts == []


def test_push_sends_begin_then_the_bytes_then_end():
    body = firmware()
    with client(device_token=TOKEN) as c:
        login(c)
        c.app.state.devices.reply_timeout_s = 0.2
        fake, _ = connected(c)

        c.post("/firmware/push", content=body)

    assert fake.frames[0]["type"] == "ota_begin"
    assert fake.frames[0]["size"] == len(body)
    assert fake.frames[0]["version"] == "v0.2.1"
    assert fake.frames[-1]["type"] == "ota_end"
    assert bytes(fake.blob) == body
    # Order matters as much as content: an image written before its begin
    # would land in a partition nobody had opened.
    assert fake.order[0] == "text"
    assert fake.order[-1] == "text"


def test_push_never_sends_a_frame_larger_than_the_devices_buffer():
    body = firmware(size=OTA_CHUNK * 2 + 5)
    with client(device_token=TOKEN) as c:
        login(c)
        c.app.state.devices.reply_timeout_s = 0.2
        fake, _ = connected(c)

        c.post("/firmware/push", content=body)

    # The device's own receive buffer is 4096 - ws_start's .buffer_size. A
    # larger frame arrives fragmented, which the firmware handles, but there
    # is no reason to make one and every reason not to guess.
    assert fake.binary_sizes == [OTA_CHUNK, OTA_CHUNK, 5]
    assert sum(fake.binary_sizes) == len(body)


def test_push_streams_progress_and_the_outcome():
    body = firmware()
    with client(device_token=TOKEN) as c:
        login(c)
        _, link = connected(c)
        # Answered before it is asked. put_nowait with no waiter needs no
        # loop, and await_outcome finds it there.
        link.replies.put_nowait({"type": "ota_ready"})

        r = c.post("/firmware/push", content=body)

    assert r.status_code == 200
    reported = lines(r)
    sent = [line["sent"] for line in reported if "sent" in line]
    assert sent == sorted(sent)
    assert sent[0] == 0, "progress should start at zero, not at the first chunk"
    assert sent[-1] == len(body)
    assert reported[-1] == {"done": True, "outcome": "ota_ready", "reason": ""}


def test_push_reports_a_refusal_from_the_device():
    with client(device_token=TOKEN) as c:
        login(c)
        _, link = connected(c)
        link.replies.put_nowait({"type": "ota_failed", "reason": "busy talking"})

        r = c.post("/firmware/push", content=firmware())

    assert lines(r)[-1] == {"done": True, "outcome": "ota_failed", "reason": "busy talking"}


def test_push_ignores_an_ota_frame_it_does_not_understand():
    """A frame that is neither ready nor failed must not end the wait.

    Otherwise a future firmware sending, say, ota_progress would make every
    push report success the moment it arrived.
    """
    with client(device_token=TOKEN) as c:
        login(c)
        c.app.state.devices.reply_timeout_s = 0.3
        _, link = connected(c)
        link.replies.put_nowait({"type": "ota_something_new"})

        r = c.post("/firmware/push", content=firmware())

    assert lines(r)[-1]["outcome"] == "timeout"


def test_push_gives_up_waiting_rather_than_hanging():
    with client(device_token=TOKEN) as c:
        login(c)
        c.app.state.devices.reply_timeout_s = 0.3
        connected(c)

        r = c.post("/firmware/push", content=firmware())  # nobody ever answers

    assert lines(r)[-1]["outcome"] == "timeout"


def test_a_socket_that_dies_mid_transfer_is_reported_not_raised():
    with client(device_token=TOKEN) as c:
        login(c)
        fake, _ = connected(c, BrokenWebSocket())

        r = c.post("/firmware/push", content=firmware())

    # The response is a stream that already carried a 200 header, so the
    # failure has to arrive inside the body rather than as a status.
    assert r.status_code == 200
    assert lines(r)[-1]["outcome"] == "error"
    assert "went away" in lines(r)[-1]["reason"]


def test_the_push_slot_is_released_after_a_failure():
    """A transfer that died must not lock the device out of the next one."""
    with client(device_token=TOKEN) as c:
        login(c)
        connected(c, BrokenWebSocket())
        c.post("/firmware/push", content=firmware())
        assert c.app.state.devices.pushing is False, "the slot was never released"

        # And a second attempt is refused for a real reason, not for that one.
        fake, _ = connected(c)
        c.app.state.devices.reply_timeout_s = 0.2
        assert c.post("/firmware/push", content=firmware()).status_code == 200


def test_a_push_while_the_slot_is_taken_is_a_409():
    with client(device_token=TOKEN) as c:
        login(c)
        connected(c)
        # Exactly the state a push in flight leaves the registry in.
        assert c.app.state.devices.begin_push() is True
        r = c.post("/firmware/push", content=firmware())
        assert r.status_code == 409
        assert "already" in r.text


# ------------------------------------------------------------ what it knows


def test_device_endpoint_reports_nothing_when_nothing_is_connected():
    with client(device_token=TOKEN) as c:
        login(c)
        assert c.get("/firmware/device").json() == {"online": False, "version": None}


def test_device_endpoint_reports_what_hello_said():
    with client(device_token=TOKEN) as c:
        login(c)
        _, link = connected(c)
        c.app.state.devices.on_frame("esp32", {"type": "hello", "version": "v0.3.0-2-gabc1234"})
        assert c.get("/firmware/device").json() == {
            "online": True,
            "version": "v0.3.0-2-gabc1234",
        }
        assert link.version == "v0.3.0-2-gabc1234"


# ------------------------------------------------------------- the registry
#
# Tested directly rather than through a websocket, for the reason in the
# module docstring. What a real socket proves and this does not - that the
# frames actually reach here from `/ws` - is covered by the end-to-end run
# with scripts/fake_device.py.


def test_registry_takes_hello_and_every_ota_frame():
    registry = DeviceRegistry()
    link = registry.register("esp32", FakeWebSocket())

    assert registry.on_frame("esp32", {"type": "hello", "version": "v1"}) is True
    assert link.version == "v1"

    assert registry.on_frame("esp32", {"type": "ota_ready"}) is True
    assert registry.on_frame("esp32", {"type": "ota_failed", "reason": "x"}) is True
    assert link.replies.qsize() == 2


def test_registry_leaves_the_sessions_own_frames_alone():
    registry = DeviceRegistry()
    registry.register("esp32", FakeWebSocket())
    for frame in ({"type": "start"}, {"type": "end"}, {"type": "cancel"}, {"type": "text"}):
        assert registry.on_frame("esp32", frame) is False, frame


def test_registry_ignores_a_hello_with_no_usable_version():
    registry = DeviceRegistry()
    link = registry.register("esp32", FakeWebSocket())
    assert registry.on_frame("esp32", {"type": "hello", "version": 7}) is True
    assert link.version is None


def test_registry_reports_only_the_surface_asked_for():
    registry = DeviceRegistry()
    registry.register("web", FakeWebSocket())
    assert registry.get("esp32") is None, "a browser was mistaken for the device"
    assert registry.get("web") is not None


def test_registry_drops_only_its_own_link():
    """A reconnect that raced a teardown has already replaced the entry, and
    dropping it then would deregister a device that is live."""
    registry = DeviceRegistry()
    first = registry.register("esp32", FakeWebSocket())
    second = registry.register("esp32", FakeWebSocket())
    registry.drop("esp32", first)
    assert registry.get("esp32") is second
    registry.drop("esp32", second)
    assert registry.get("esp32") is None


def test_registry_push_slot_is_claimed_once_and_comes_back():
    registry = DeviceRegistry()
    assert registry.pushing is False
    assert registry.begin_push() is True
    assert registry.begin_push() is False, "a second push claimed the slot"
    registry.end_push()
    assert registry.begin_push() is True, "the slot did not come back"


def test_registry_ignores_frames_for_a_surface_it_does_not_know():
    registry = DeviceRegistry()
    assert registry.on_frame("esp32", {"type": "hello", "version": "v1"}) is False


# ------------------------------------------------- and through a real socket
#
# The one thing a fake link cannot show: that `/ws` registers it at all and
# that main.py's loop routes `hello` to it before the session dispatch. Both
# of these read the registry in-process rather than over HTTP, which is what
# keeps them clear of the portal contention the module docstring describes.


def test_the_ws_endpoint_registers_the_device_and_routes_hello():
    with client(device_token=TOKEN) as c:
        with c.websocket_connect("/ws", headers=DEVICE_HEADERS) as ws:
            ws.send_text(json.dumps({"type": "hello", "version": "v0.9.9"}))
            # A round trip on the same socket is the only ordering guarantee
            # available, so ask for something the session itself answers.
            ws.send_text(json.dumps({"type": "start"}))
            assert json.loads(ws.receive_text())["value"] == "listening"

            link = c.app.state.devices.get("esp32")
            assert link is not None, "/ws did not register the device"
            assert link.version == "v0.9.9", "hello never reached the registry"


def test_a_browser_connection_is_not_registered_as_the_device():
    with client(device_token=TOKEN) as c:
        login(c)
        # No bearer token, but a session cookie: _authorise_connection calls
        # this "web", and it must not become the thing firmware is pushed to.
        with c.websocket_connect("/ws") as ws:
            ws.send_text(json.dumps({"type": "hello", "version": "a browser"}))
            ws.send_text(json.dumps({"type": "start"}))
            assert json.loads(ws.receive_text())["value"] == "listening"

            assert c.app.state.devices.get("esp32") is None
            assert c.app.state.devices.get("web") is not None


def test_the_device_going_away_deregisters_it():
    with client(device_token=TOKEN) as c:
        with c.websocket_connect("/ws", headers=DEVICE_HEADERS) as ws:
            ws.send_text(json.dumps({"type": "start"}))
            assert json.loads(ws.receive_text())["value"] == "listening"
            assert c.app.state.devices.get("esp32") is not None
        assert c.app.state.devices.get("esp32") is None


def test_an_ota_frame_does_not_reach_the_session():
    """ota_* and hello are intercepted before session dispatch. Without that
    they land in main.py's "ignoring control message" branch, which is
    harmless today and would silently swallow the reply this feature needs."""
    with client(device_token=TOKEN) as c:
        with c.websocket_connect("/ws", headers=DEVICE_HEADERS) as ws:
            ws.send_text(json.dumps({"type": "ota_failed", "reason": "nobody asked"}))
            ws.send_text(json.dumps({"type": "start"}))
            assert json.loads(ws.receive_text())["value"] == "listening"

            link = c.app.state.devices.get("esp32")
            assert link.replies.qsize() == 1, "the frame did not reach the relay"
