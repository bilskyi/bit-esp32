"""Carrying a firmware image to the device, and storing none of it.

The device already holds an authenticated WebSocket to this server, so the
image goes down that same socket. Two things follow from that, and both are
the point rather than a side effect:

- There is no second TLS session to pay for. The board has roughly 30 KB of
  free heap once ``wss://`` is up, and a handshake wants tens of KB
  transiently - see the note above ``MIC_BUFFER_BYTES`` in voice_main.c.
- This server never hosts a binary. It relays one. Nothing is written to
  disk, no volume or bucket is needed, and nobody who runs their own copy of
  this server has to depend on anybody else's.

Releases live on GitHub, the browser downloads them from there, and the
device never talks to GitHub at all.
"""

import asyncio
import json
import logging
import struct

from fastapi import Depends, FastAPI, HTTPException, Request, WebSocket
from fastapi.responses import StreamingResponse

log = logging.getLogger(__name__)

# The device's own receive buffer is 4096 - `.buffer_size` in ws_start(). A
# larger frame arrives fragmented, which the firmware handles, but there is
# no reason to make one.
OTA_CHUNK = 4096

# How much of a file is needed to judge it: esp_app_desc_t.project_name sits
# at 0x50 and date[16] ends at 0x8F. Same constant as OL_HEADER_MIN.
HEADER_MIN = 144

PROJECT_NAME = b"voice_capture"
CHIP_ID_ESP32C3 = 5

# Long enough for the device to finish writing 1 MB to flash and answer,
# short enough that a device which has silently gone away does not hold the
# request open. Overridable on the registry, which is how the tests keep
# themselves quick.
OTA_REPLY_TIMEOUT_S = 30.0

# A megabyte is the real size; four is room to grow without letting a stray
# upload become a memory problem. The body is held in memory rather than
# streamed straight through, because the device needs the total size in
# ota_begin before the first byte - esp_ota_begin(OTA_SIZE_UNKNOWN) erases
# the whole 1.94 MB partition up front instead of lazily by sector.
MAX_BODY = 4 * 1024 * 1024

DEVICE_SURFACE = "esp32"


def check_image(head: bytes) -> str | None:
    """Whether these bytes begin a firmware image for this project.

    The same four offsets as ``ol_check_image()`` in the firmware, read out
    of a real build rather than inferred. None means valid; anything else is
    a reason fit to show a person.
    """
    if len(head) < HEADER_MIN:
        return "too short to be firmware"
    if head[0] != 0xE9:
        return "not an ESP firmware image"
    if struct.unpack_from("<H", head, 0x0C)[0] != CHIP_ID_ESP32C3:
        return "built for another chip"
    if struct.unpack_from("<I", head, 0x20)[0] != 0xABCD5432:
        return "no application descriptor"
    # Over the terminator, so a name that merely starts with ours -
    # "voice_capture2" is a different project - does not pass.
    if head[0x50 : 0x50 + len(PROJECT_NAME) + 1] != PROJECT_NAME + b"\0":
        return "firmware for another project"
    return None


def image_version(head: bytes) -> str | None:
    """esp_app_desc_t.version, or None if the field is malformed.

    Thirty-two bytes with no terminator is not a long version string, it is
    a broken image - the same judgement ol_image_version() makes.
    """
    if len(head) < HEADER_MIN:
        return None
    field = head[0x30:0x50]
    end = field.find(b"\0")
    if end < 0:
        return None
    return field[:end].decode("ascii", "replace")


class DeviceLink:
    """One live connection, and the frames it sends back about updates."""

    def __init__(self, ws: WebSocket) -> None:
        self.ws = ws
        self.version: str | None = None
        self.replies: asyncio.Queue[dict] = asyncio.Queue()


class DeviceRegistry:
    """Which surfaces are connected right now, and nothing more.

    Deliberately not a device registry in the fuller sense: no heartbeat, no
    uptime, no history, no rows that outlive a connection. Devices.tsx says
    outright that none of that is reported, and this keeps that true while
    still being enough to push an image at the thing.
    """

    def __init__(self) -> None:
        self._links: dict[str, DeviceLink] = {}
        self._pushing = False
        self.reply_timeout_s = OTA_REPLY_TIMEOUT_S

    def register(self, surface: str, ws: WebSocket) -> DeviceLink:
        if surface in self._links:
            # Two devices sharing a token is not supported and not silently
            # tolerated either: the newest wins, and the log says so.
            log.warning("a second %s connected; the newer one is now the one addressed", surface)
        link = DeviceLink(ws)
        self._links[surface] = link
        return link

    def drop(self, surface: str, link: DeviceLink) -> None:
        # Only if it is still ours. A reconnect that raced this teardown has
        # already replaced the entry, and dropping it would deregister a live
        # device.
        if self._links.get(surface) is link:
            del self._links[surface]

    def get(self, surface: str) -> DeviceLink | None:
        return self._links.get(surface)

    @property
    def pushing(self) -> bool:
        return self._pushing

    def begin_push(self) -> bool:
        """Claim the one push slot. False when it is already taken.

        A flag rather than an asyncio.Lock, and that is not laziness:
        everything here runs on one event loop, so there is no window between
        the test and the set, and a Lock would have to be acquired in the
        route and released inside a streaming generator.
        """
        if self._pushing:
            return False
        self._pushing = True
        return True

    def end_push(self) -> None:
        self._pushing = False

    def on_frame(self, surface: str, control: dict) -> bool:
        """Take the frames that belong to this module. True if consumed.

        Called before the session dispatch in main.py, because that dispatch
        would drop these into "ignoring control message".
        """
        kind = control.get("type")
        link = self._links.get(surface)
        if link is None:
            return False

        if kind == "hello":
            version = control.get("version")
            link.version = version if isinstance(version, str) else None
            log.info("%s says it is running %s", surface, link.version)
            return True

        if isinstance(kind, str) and kind.startswith("ota_"):
            link.replies.put_nowait(control)
            return True

        return False

    async def await_outcome(self, link: DeviceLink) -> dict:
        """The device's verdict, or a timeout of our own making."""
        loop = asyncio.get_running_loop()
        deadline = loop.time() + self.reply_timeout_s
        while True:
            remaining = deadline - loop.time()
            if remaining <= 0:
                return {"type": "timeout"}
            try:
                frame = await asyncio.wait_for(link.replies.get(), remaining)
            except (asyncio.TimeoutError, TimeoutError):
                return {"type": "timeout"}
            if frame.get("type") in ("ota_ready", "ota_failed"):
                return frame
            # Anything else on this queue is an ota_* frame this version does
            # not know. Keep waiting for one that ends the transfer.


def register_firmware_routes(app: FastAPI, require_login) -> None:
    """Two routes: push an image at the device, and ask what it is running."""

    @app.post("/firmware/push", dependencies=[Depends(require_login)])
    async def push(request: Request) -> StreamingResponse:
        registry: DeviceRegistry = app.state.devices

        link = registry.get(DEVICE_SURFACE)
        if link is None:
            raise HTTPException(status_code=409, detail="no device is connected")

        # Cheap and early, so a second push does not read a megabyte before
        # being turned away. The authoritative claim is begin_push() below,
        # after the file has been judged - this one is only an optimisation
        # and is allowed to be racy.
        if registry.pushing:
            raise HTTPException(status_code=409, detail="an update is already running")

        # Before reading a byte of it. Checked on the declared length rather
        # than on what arrived, because the point is to refuse an absurd
        # upload without first pulling it into memory - the body is held
        # whole, and "read it all, then object to the size" is not a check.
        declared = request.headers.get("content-length")
        if declared is not None and declared.isdigit() and int(declared) > MAX_BODY:
            raise HTTPException(status_code=413, detail="far larger than any firmware image")

        body = await request.body()
        if len(body) > MAX_BODY:
            raise HTTPException(status_code=413, detail="far larger than any firmware image")
        reason = check_image(body)
        if reason is not None:
            # Refused before the device hears a word of it: a wrong file must
            # not cost the running image its partition.
            raise HTTPException(status_code=400, detail=reason)

        total = len(body)
        version = image_version(body) or ""
        if not registry.begin_push():
            raise HTTPException(status_code=409, detail="an update is already running")

        async def pump():
            try:
                log.warning("pushing %d bytes of firmware %s to %s", total, version, DEVICE_SURFACE)
                await link.ws.send_text(
                    json.dumps({"type": "ota_begin", "size": total, "version": version})
                )
                yield json.dumps({"sent": 0, "total": total}) + "\n"

                sent = 0
                for start in range(0, total, OTA_CHUNK):
                    await link.ws.send_bytes(body[start : start + OTA_CHUNK])
                    sent = min(start + OTA_CHUNK, total)
                    yield json.dumps({"sent": sent, "total": total}) + "\n"

                await link.ws.send_text(json.dumps({"type": "ota_end"}))

                outcome = await registry.await_outcome(link)
                yield json.dumps(
                    {
                        "done": True,
                        "outcome": outcome.get("type"),
                        "reason": outcome.get("reason", ""),
                    }
                ) + "\n"
                log.warning("push finished: %s", outcome.get("type"))
            except Exception as err:  # the socket died mid-transfer
                log.exception("push failed")
                try:
                    await link.ws.send_text(json.dumps({"type": "ota_abort"}))
                except Exception:
                    pass  # it is already gone; that is why we are here
                yield json.dumps(
                    {"done": True, "outcome": "error", "reason": str(err) or type(err).__name__}
                ) + "\n"
            finally:
                registry.end_push()

        # Newline-delimited JSON, read by the browser as it arrives. The
        # browser's own XHR upload progress covers getting the file here;
        # this covers getting it to the device, which is the slow half.
        #
        # Content-Encoding: identity is not decoration. GZipMiddleware is
        # installed with minimum_size=1000 and compresses streaming bodies
        # chunk by chunk through a GzipFile, which does not emit anything
        # until its own buffer fills - so the progress lines would arrive in
        # batches, or all at the end, and a progress bar that only moves once
        # is worse than none. The middleware passes through any response that
        # already declares an encoding.
        return StreamingResponse(
            pump(),
            media_type="application/x-ndjson",
            headers={"Content-Encoding": "identity"},
        )

    @app.get("/firmware/device", dependencies=[Depends(require_login)])
    async def device() -> dict:
        link: DeviceLink | None = app.state.devices.get(DEVICE_SURFACE)
        return {
            "online": link is not None,
            "version": link.version if link is not None else None,
        }
