"""The relay that carries a firmware image to the device.

The push tests run the POST on a thread. They have to: the request does not
finish until the device has answered, and the device is this same test.
"""

import json
import struct
import threading

from tests.test_main import client  # noqa: F401 - see the note below

# client() lives in test_main.py rather than conftest.py, and is imported
# rather than copied. Moving it to conftest would be the tidier home but
# touches forty-odd call sites in test_main.py for no behaviour change, and
# copying it here would leave two of them to drift apart.

from server.firmware import (
    CHIP_ID_ESP32C3,
    HEADER_MIN,
    MAX_BODY,
    OTA_CHUNK,
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


def login(c):
    r = c.post("/login", json={"username": "test", "password": "test123"})
    assert r.status_code == 200


def push_on_a_thread(c, body, out):
    def run():
        out.append(c.post("/firmware/push", content=body))

    t = threading.Thread(target=run)
    t.start()
    return t


def device_frames(ws, until="ota_end", limit=200):
    """Everything the server sends the device, up to and including `until`."""
    texts, blob = [], bytearray()
    for _ in range(limit):
        message = ws.receive()
        if message.get("bytes") is not None:
            blob += message["bytes"]
            continue
        text = message.get("text")
        if text is None:
            continue
        payload = json.loads(text)
        texts.append(payload)
        if payload.get("type") == until:
            break
    return texts, bytes(blob)


def test_push_requires_a_login():
    with client(device_token=TOKEN) as c:
        assert c.post("/firmware/push", content=firmware()).status_code == 401


def test_push_with_no_device_connected_is_a_409():
    with client(device_token=TOKEN) as c:
        login(c)
        r = c.post("/firmware/push", content=firmware())
        assert r.status_code == 409
        assert "device" in r.text


def test_push_refuses_a_file_that_is_not_firmware():
    with client(device_token=TOKEN) as c:
        login(c)
        with c.websocket_connect("/ws", headers=DEVICE_HEADERS) as ws:
            bad = bytearray(firmware())
            bad[0] = ord("P")
            r = c.post("/firmware/push", content=bytes(bad))
            assert r.status_code == 400
            assert "ESP" in r.text
            # And the device was told nothing at all: a refused file must not
            # cost the running image its partition. Asserted directly rather
            # than by asking the server what it thinks - if an ota_begin had
            # gone out, it would be sitting in front of this reply.
            ws.send_text(json.dumps({"type": "start"}))
            assert json.loads(ws.receive_text())["value"] == "listening"


def test_push_refuses_an_empty_body():
    with client(device_token=TOKEN) as c:
        login(c)
        with c.websocket_connect("/ws", headers=DEVICE_HEADERS):
            assert c.post("/firmware/push", content=b"").status_code == 400


def test_push_refuses_something_far_too_large_to_be_firmware():
    with client(device_token=TOKEN) as c:
        login(c)
        with c.websocket_connect("/ws", headers=DEVICE_HEADERS):
            # A valid header on the front, so the only thing wrong with it is
            # the size - otherwise this would pass for the wrong reason.
            oversized = firmware(size=MAX_BODY + 1)
            r = c.post("/firmware/push", content=oversized)
            assert r.status_code == 413
            assert "larger" in r.text


def test_push_sends_begin_then_the_bytes_then_end():
    body = firmware()
    with client(device_token=TOKEN) as c:
        login(c)
        with c.websocket_connect("/ws", headers=DEVICE_HEADERS) as ws:
            out = []
            t = push_on_a_thread(c, body, out)
            texts, blob = device_frames(ws)
            ws.send_text(json.dumps({"type": "ota_ready"}))
            t.join(timeout=20)

    assert texts[0]["type"] == "ota_begin"
    assert texts[0]["size"] == len(body)
    assert texts[0]["version"] == "v0.2.1"
    assert texts[-1]["type"] == "ota_end"
    assert blob == body


def test_push_never_sends_a_frame_larger_than_the_devices_buffer():
    body = firmware(size=OTA_CHUNK * 2 + 5)
    with client(device_token=TOKEN) as c:
        login(c)
        with c.websocket_connect("/ws", headers=DEVICE_HEADERS) as ws:
            out = []
            t = push_on_a_thread(c, body, out)
            sizes = []
            for _ in range(200):
                message = ws.receive()
                if message.get("bytes") is not None:
                    sizes.append(len(message["bytes"]))
                    continue
                if message.get("text") and json.loads(message["text"])["type"] == "ota_end":
                    break
            ws.send_text(json.dumps({"type": "ota_ready"}))
            t.join(timeout=20)

    # The device's own receive buffer is 4096 (ws_start's .buffer_size). A
    # larger frame arrives fragmented, which still works, but there is no
    # reason to make it and every reason not to guess.
    assert sizes and max(sizes) <= OTA_CHUNK
    assert sum(sizes) == len(body)


def test_push_streams_progress_and_the_outcome():
    body = firmware()
    with client(device_token=TOKEN) as c:
        login(c)
        with c.websocket_connect("/ws", headers=DEVICE_HEADERS) as ws:
            out = []
            t = push_on_a_thread(c, body, out)
            device_frames(ws)
            ws.send_text(json.dumps({"type": "ota_ready"}))
            t.join(timeout=20)

    assert out and out[0].status_code == 200
    lines = [json.loads(line) for line in out[0].text.splitlines() if line.strip()]
    sent = [line["sent"] for line in lines if "sent" in line]
    assert sent == sorted(sent)
    assert sent[-1] == len(body)
    assert lines[-1]["done"] is True
    assert lines[-1]["outcome"] == "ota_ready"


def test_push_reports_a_refusal_from_the_device():
    body = firmware()
    with client(device_token=TOKEN) as c:
        login(c)
        with c.websocket_connect("/ws", headers=DEVICE_HEADERS) as ws:
            out = []
            t = push_on_a_thread(c, body, out)
            device_frames(ws)
            ws.send_text(json.dumps({"type": "ota_failed", "reason": "busy talking"}))
            t.join(timeout=20)

    lines = [json.loads(line) for line in out[0].text.splitlines() if line.strip()]
    assert lines[-1]["outcome"] == "ota_failed"
    assert lines[-1]["reason"] == "busy talking"


def test_push_gives_up_waiting_rather_than_hanging():
    body = firmware()
    with client(device_token=TOKEN) as c:
        login(c)
        c.app.state.devices.reply_timeout_s = 0.3
        with c.websocket_connect("/ws", headers=DEVICE_HEADERS) as ws:
            device_frames_thread = []
            t = push_on_a_thread(c, body, device_frames_thread)
            device_frames(ws)  # read, but never answer
            t.join(timeout=20)

    lines = [
        json.loads(line) for line in device_frames_thread[0].text.splitlines() if line.strip()
    ]
    assert lines[-1]["outcome"] == "timeout"


def test_a_second_push_while_one_is_running_is_a_409():
    body = firmware()
    with client(device_token=TOKEN) as c:
        login(c)
        with c.websocket_connect("/ws", headers=DEVICE_HEADERS) as ws:
            out = []
            t = push_on_a_thread(c, body, out)
            device_frames(ws)  # the first push is now waiting for a reply
            second = c.post("/firmware/push", content=body)
            assert second.status_code == 409
            assert "already" in second.text
            ws.send_text(json.dumps({"type": "ota_ready"}))
            t.join(timeout=20)


# ------------------------------------------------------------ what it knows


def test_device_endpoint_reports_nothing_when_nothing_is_connected():
    with client(device_token=TOKEN) as c:
        login(c)
        r = c.get("/firmware/device")
        assert r.json() == {"online": False, "version": None}


def test_device_endpoint_reports_what_hello_said():
    with client(device_token=TOKEN) as c:
        login(c)
        with c.websocket_connect("/ws", headers=DEVICE_HEADERS) as ws:
            ws.send_text(json.dumps({"type": "hello", "version": "v0.3.0-2-gabc1234"}))
            # The hello has to be processed before the GET can see it, and the
            # only ordering guarantee available is a round trip on the same
            # socket - so ask for something the session answers.
            ws.send_text(json.dumps({"type": "start"}))
            assert json.loads(ws.receive_text())["value"] == "listening"
            r = c.get("/firmware/device")
            assert r.json() == {"online": True, "version": "v0.3.0-2-gabc1234"}


def test_device_endpoint_requires_a_login():
    with client(device_token=TOKEN) as c:
        assert c.get("/firmware/device").status_code == 401


def test_a_browser_connection_is_not_mistaken_for_the_device():
    with client(device_token=TOKEN) as c:
        login(c)
        # No bearer token, but a session cookie: _authorise_connection calls
        # this "web", and it must not register as the thing firmware is
        # pushed to.
        with c.websocket_connect("/ws") as ws:
            ws.send_text(json.dumps({"type": "hello", "version": "a browser"}))
            ws.send_text(json.dumps({"type": "start"}))
            assert json.loads(ws.receive_text())["value"] == "listening"
            assert c.get("/firmware/device").json()["online"] is False
            assert c.post("/firmware/push", content=firmware()).status_code == 409


def test_the_device_going_away_deregisters_it():
    with client(device_token=TOKEN) as c:
        login(c)
        with c.websocket_connect("/ws", headers=DEVICE_HEADERS) as ws:
            ws.send_text(json.dumps({"type": "hello", "version": "v1"}))
            ws.send_text(json.dumps({"type": "start"}))
            assert json.loads(ws.receive_text())["value"] == "listening"
            assert c.get("/firmware/device").json()["online"] is True
        assert c.get("/firmware/device").json() == {"online": False, "version": None}


def test_an_ota_frame_does_not_reach_the_session():
    """ota_* and hello are intercepted before session dispatch. Without that
    they land in main.py's "ignoring control message" branch, which is
    harmless today and would silently swallow the reply this feature needs."""
    with client(device_token=TOKEN) as c:
        login(c)
        with c.websocket_connect("/ws", headers=DEVICE_HEADERS) as ws:
            ws.send_text(json.dumps({"type": "ota_failed", "reason": "nobody asked"}))
            ws.send_text(json.dumps({"type": "start"}))
            assert json.loads(ws.receive_text())["value"] == "listening"
