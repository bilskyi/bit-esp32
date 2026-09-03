// Writing a new image to the other slot, and deciding whether to keep it.
//
// Everything decidable without hardware is in ota_logic.h; this file is the
// part that can only be judged on the board. It stays thin for that reason.
//
// Two rules that are easy to lose and expensive to lose:
//
// 1. ota_boot_guard() is the FIRST statement of app_main(). Everything after
//    it is something that can hang, and a deadline armed after the hang is
//    not a deadline. This project's task watchdog does not panic
//    (CONFIG_ESP_TASK_WDT_PANIC is unset), so a hang does not reboot on its
//    own and nothing else would ever take a bad image back.
//
// 2. Provisioning calls ota_deadline_suspend(true), never ota_mark_valid().
//    An image that breaks NVS reading would land in provisioning, declare
//    itself good, and take the working firmware with it. It does not need to:
//    a provisioning session that succeeds ends in a reboot and a connection
//    to the server, which is what ota_mark_valid() is for.
//
// Concurrency: there is exactly one transfer at a time, and that is enforced
// by the state machine rather than by a lock - ol_xfer_begin() returns
// OL_STEP_BUSY to the second caller. A mutex here would have to be held
// across a megabyte of flash writes, which is worse than the problem.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// How long a new image has to reach the server before it is taken back.
//
// Not 90 seconds. link_task already reboots the board after that long
// offline, and while an image is unproven that reboot *is* a rollback - so a
// router that takes longer than a minute and a half to come back would undo
// a perfectly healthy update. Ten minutes is long enough for a router to
// finish rebooting and short enough that a device which will never come back
// does not sit there forever.
#define OTA_DEADLINE_MS (10 * 60 * 1000)

// ------------------------------------------------------- the rollback side

// Reads the running partition's OTA state and, if this image is unproven,
// starts the deadline that will take it back. Does nothing at all for an
// image flashed over the cable: that one has no otadata entry and must never
// be rolled back.
void ota_boot_guard(void);

// True while the running image is unproven. link_task consults this before
// its own reboot, because in this state a reboot is a rollback.
bool ota_pending_verify(void);

// Keeps the running image. Idempotent and cheap, so calling it on every
// frame that arrives from the server is the intended use: a frame from the
// server proves radio, TLS, token and protocol at once, which is the whole
// definition of a good image.
void ota_mark_valid(const char *why);

// Stops and restarts the deadline clock without deciding anything. Resuming
// grants a full fresh OTA_DEADLINE_MS, because somebody who just finished
// provisioning deserves the whole window to get online.
void ota_deadline_suspend(bool on);

// ------------------------------------------------------- the transfer side

// esp_app_get_description()->version of the running image - `git describe`
// output, so it can be `v0.3.0`, `v0.3.0-2-gabc1234-dirty`, or a bare hash
// when the repository has no tags. Never NULL.
const char *ota_running_version(void);

// The size of the slot a new image would be written to.
uint32_t ota_capacity(void);

// Opens a transfer. `size` is mandatory and is not merely validated:
// esp_ota_begin(OTA_SIZE_UNKNOWN) erases the whole 1.94 MB partition up
// front, which is seconds of blocking, while a known size erases lazily by
// sector.
esp_err_t ota_begin(uint32_t size);

// Feeds bytes in arrival order. The first OL_HEADER_MIN of them are held
// back and checked with ol_check_image() before any of them reach the flash,
// so a file that is not firmware for this project on this chip costs nothing.
esp_err_t ota_write(const void *data, size_t len);

// Verifies and commits. Both esp_ota_end() and esp_ota_set_boot_partition()
// run esp_image_verify(), which checks the SHA-256 the image carries in its
// own last 32 bytes - which is why there is no hash in the protocol.
esp_err_t ota_end(void);

// Safe to call at any time, including when there is nothing to abort.
void ota_abort(void);

bool ota_active(void);
uint32_t ota_received(void);
uint32_t ota_expected(void);

// Why the last transfer failed, for the log and for the frame sent back to
// whoever asked for the update. Never NULL; empty when nothing has failed.
//
// Every string that reaches it comes from ol_step_text(), ol_verdict_text(),
// esp_err_to_name() or ota_note_error() below - fixed literals, none of them
// carrying a quote or a backslash. That is what lets a caller drop it
// straight into a JSON frame without escaping it. A future reason with a
// quote in it would break that frame, so keep them plain.
const char *ota_last_error(void);

// Records a refusal this module did not make itself - the caller declining an
// update because the device is mid-conversation, say. Keeps the reason where
// the rest of them live, so the frame sent back always has one.
void ota_note_error(const char *why);
