// The parts of updating that are pure functions over bytes.
//
// Split out of ota.c for one reason: host/ can build this, and the image
// header check, the frame parser and the transfer bookkeeping are where the
// bugs actually are. Everything here follows face.c's rules - no ESP-IDF
// header, no float, no allocation - so the laptop can run it.
//
// Two of these are parsing attacker-supplied bytes in the ordinary case: the
// image header comes from a file somebody chose, and the control frames come
// off a socket. The sanitizer build in host/Makefile is not optional for
// this file.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------- the image

// How much of a file is needed to judge it. esp_app_desc_t's project_name
// sits at 0x50..0x6F and date[16] ends at 0x8F, so 0x90 bytes covers every
// field checked below with nothing to spare and nothing wasted.
#define OL_HEADER_MIN 144

// From project() in firmware/CMakeLists.txt. A binary built from a different
// project is a valid ESP32-C3 image and would flash and boot perfectly - into
// something that is not this device.
#define OL_PROJECT_NAME "voice_capture"

// esp_chip_id_t: ESP_CHIP_ID_ESP32C3.
#define OL_CHIP_ID_ESP32C3 5

// Offsets inside the image, read out of a real build rather than inferred:
//   0x00  esp_image_header_t.magic          0xE9
//   0x0C  esp_image_header_t.chip_id        uint16, little-endian
//   0x20  esp_app_desc_t.magic_word         0xABCD5432
//   0x30  esp_app_desc_t.version[32]
//   0x50  esp_app_desc_t.project_name[32]
#define OL_OFF_IMAGE_MAGIC 0x00
#define OL_OFF_CHIP_ID 0x0C
#define OL_OFF_APP_DESC 0x20
#define OL_OFF_VERSION 0x30
#define OL_OFF_PROJECT 0x50
#define OL_DESC_FIELD 32

typedef enum {
    OL_IMAGE_OK = 0,
    OL_IMAGE_TOO_SHORT,      // fewer than OL_HEADER_MIN bytes to judge
    OL_IMAGE_NOT_ESP,        // byte 0 is not 0xE9 - a zip, a text file, junk
    OL_IMAGE_WRONG_CHIP,     // built for another part
    OL_IMAGE_NO_APP_DESC,    // no esp_app_desc_t where one must be
    OL_IMAGE_WRONG_PROJECT,  // a real image, for something else
} ol_image_verdict_t;

// Judges a file from its first bytes alone, so a wrong one is refused before
// a single byte reaches the flash. Never reads past len.
ol_image_verdict_t ol_check_image(const unsigned char *head, size_t len);

const char *ol_verdict_text(ol_image_verdict_t v);

// Reads esp_app_desc_t.version out of the header. False when the buffer is
// too short or the field carries no terminator inside its 32 bytes - which is
// a malformed image, not a long version string. Writes at most out_size-1
// bytes and always terminates.
bool ol_image_version(const unsigned char *head, size_t len, char *out, size_t out_size);

// ---------------------------------------------------------- the frames

typedef enum {
    OL_FRAME_OTHER = 0,
    OL_FRAME_BEGIN,
    OL_FRAME_END,
    OL_FRAME_ABORT,
} ol_frame_t;

// The type of a control frame, matched as the *value of the "type" key* and
// never as a substring anywhere in the body.
//
// voice_main.c's existing handler matches "emotion", "done" and "speaking"
// with memmem, which is fine for a closed vocabulary the server controls. It
// is not fine here: {"type":"text","value":"ota_begin"} is a legitimate frame
// carrying a spoken sentence, and a substring match on it would reflash the
// device. Safe on a body that is not NUL-terminated, which is what the
// websocket client hands over.
ol_frame_t ol_frame_type(const char *body, size_t len);

// Reads an unsigned integer field. False when it is absent, is not a number,
// or does not fit in 32 bits. *out is left alone on failure.
bool ol_field_u32(const char *body, size_t len, const char *name, uint32_t *out);

// ---------------------------------------------------- the transfer itself

typedef enum {
    OL_XFER_IDLE = 0,
    OL_XFER_ACTIVE,
    OL_XFER_DONE,
} ol_xfer_state_t;

typedef struct {
    ol_xfer_state_t state;
    uint32_t expected;
    uint32_t received;
} ol_xfer_t;

typedef enum {
    OL_STEP_OK = 0,
    OL_STEP_BUSY,        // a begin arrived while a transfer was running
    OL_STEP_NOT_ACTIVE,  // a write or end arrived with no transfer running
    OL_STEP_TOO_BIG,     // the promised size does not fit the partition
    OL_STEP_EMPTY,       // a promised size of zero
    OL_STEP_OVERRUN,     // more bytes arrived than were promised
    OL_STEP_SHORT,       // end arrived before the promised size did
} ol_step_t;

void ol_xfer_reset(ol_xfer_t *x);

// Every rejection except OL_STEP_BUSY leaves the transfer unusable - state
// goes back to OL_XFER_IDLE - so a caller that ignores a return value cannot
// carry on feeding a broken transfer. BUSY is the exception on purpose: a
// stray second begin must not destroy the transfer that is already running.
ol_step_t ol_xfer_begin(ol_xfer_t *x, uint32_t size, uint32_t capacity);
ol_step_t ol_xfer_write(ol_xfer_t *x, uint32_t n);
ol_step_t ol_xfer_end(ol_xfer_t *x);

const char *ol_step_text(ol_step_t s);
