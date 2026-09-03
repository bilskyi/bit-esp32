#include "ota.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ota_logic.h"

static const char *TAG = "ota";

static esp_ota_handle_t s_handle = 0;
static const esp_partition_t *s_target = NULL;
static ol_xfer_t s_xfer;

// The first bytes of the incoming image, held back until there are enough of
// them to judge. They are written the moment the verdict is ok, so nothing
// reaches the flash before the file has been recognised.
static unsigned char s_head[OL_HEADER_MIN];
static size_t s_head_got = 0;

static char s_error[80] = {0};

static volatile bool s_pending = false;
static volatile bool s_suspended = false;

static void fail(const char *why) {
    strncpy(s_error, why, sizeof(s_error) - 1);
    s_error[sizeof(s_error) - 1] = '\0';
    ESP_LOGE(TAG, "update failed: %s", s_error);
}

// ------------------------------------------------------- the rollback side

// A task rather than an esp_timer, for two reasons. The callback would do
// flash writes and then esp_restart() from the timer task, which is not
// where either belongs; and a task at this priority still gets the CPU when
// something below it is spinning without yielding, which is exactly the
// failure this deadline exists to survive. It sleeps for all but a few
// microseconds of its life, so the priority costs nothing.
static void deadline_task(void *arg) {
    (void)arg;
    uint32_t waited_ms = 0;

    while (s_pending) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (s_suspended) {
            // Not merely paused: a resumed clock starts over. Somebody
            // part-way through provisioning has not had their chance yet.
            waited_ms = 0;
            continue;
        }

        waited_ms += 1000;
        if (waited_ms >= OTA_DEADLINE_MS) {
            ESP_LOGE(TAG,
                     "unproven after %u s with no frame from the server; "
                     "rolling back to the previous image",
                     (unsigned)(waited_ms / 1000));
            vTaskDelay(pdMS_TO_TICKS(100));  // let the line reach the console
            esp_ota_mark_app_invalid_rollback_and_reboot();
            // Not reached. If it somehow is, a plain restart while still in
            // PENDING_VERIFY rolls back too.
            esp_restart();
        }
    }

    ESP_LOGI(TAG, "image proved good; the deadline is off");
    vTaskDelete(NULL);
}

void ota_boot_guard(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) return;

    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        // No otadata entry, which is what a cable-flashed image looks like.
        // Nothing to prove and nothing that could take it away.
        return;
    }

    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;

    s_pending = true;
    s_suspended = false;
    ESP_LOGW(TAG, "this image is unproven: %u minutes to reach the server",
             (unsigned)(OTA_DEADLINE_MS / 60000));

    // Priority 10 is above every task this firmware creates (audio_in is 5).
    // See deadline_task for why that is deliberate.
    if (xTaskCreate(deadline_task, "ota_deadline", 3072, NULL, 10, NULL) != pdPASS) {
        // Worse than it looks: an unproven image with no deadline stays
        // unproven forever, and the next reboot for any reason rolls it back.
        // Say so loudly rather than pretending the guard is up.
        ESP_LOGE(TAG, "deadline task failed to start; this image has no way to prove itself");
    }
}

bool ota_pending_verify(void) { return s_pending; }

void ota_mark_valid(const char *why) {
    if (!s_pending) return;

    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "could not mark the image valid: %s", esp_err_to_name(err));
        return;
    }

    // Cleared last, so a failed mark above leaves the deadline running.
    s_pending = false;
    ESP_LOGW(TAG, "image accepted (%s)", why ? why : "no reason given");
}

void ota_deadline_suspend(bool on) {
    if (!s_pending) return;
    if (s_suspended == on) return;
    s_suspended = on;
    ESP_LOGI(TAG, "rollback deadline %s", on ? "suspended" : "resumed, from the top");
}

// ------------------------------------------------------- the transfer side

const char *ota_running_version(void) {
    const esp_app_desc_t *d = esp_app_get_description();
    return d != NULL ? d->version : "unknown";
}

uint32_t ota_capacity(void) {
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    return next != NULL ? (uint32_t)next->size : 0;
}

bool ota_active(void) { return s_xfer.state == OL_XFER_ACTIVE; }
uint32_t ota_received(void) { return s_xfer.received; }
uint32_t ota_expected(void) { return s_xfer.expected; }
const char *ota_last_error(void) { return s_error; }

void ota_abort(void) {
    if (s_handle != 0) {
        esp_ota_abort(s_handle);
        s_handle = 0;
    }
    s_target = NULL;
    s_head_got = 0;
    ol_xfer_reset(&s_xfer);
}

esp_err_t ota_begin(uint32_t size) {
    s_target = esp_ota_get_next_update_partition(NULL);
    if (s_target == NULL) {
        fail("no second app partition - is the partition table the OTA one?");
        return ESP_ERR_NOT_FOUND;
    }

    const ol_step_t step = ol_xfer_begin(&s_xfer, size, (uint32_t)s_target->size);
    if (step != OL_STEP_OK) {
        fail(ol_step_text(step));
        return ESP_ERR_INVALID_ARG;
    }

    // The real size, never OTA_SIZE_UNKNOWN: see ota.h.
    const esp_err_t err = esp_ota_begin(s_target, (size_t)size, &s_handle);
    if (err != ESP_OK) {
        s_handle = 0;
        ol_xfer_reset(&s_xfer);
        fail(esp_err_to_name(err));
        return err;
    }

    s_head_got = 0;
    s_error[0] = '\0';
    ESP_LOGW(TAG, "update started: %u bytes into %s (capacity %u)",
             (unsigned)size, s_target->label, (unsigned)s_target->size);
    return ESP_OK;
}

esp_err_t ota_write(const void *data, size_t len) {
    if (s_handle == 0 || s_xfer.state != OL_XFER_ACTIVE) {
        fail("no update is running");
        return ESP_ERR_INVALID_STATE;
    }
    if (data == NULL) {
        fail("no data");
        ota_abort();
        return ESP_ERR_INVALID_ARG;
    }

    const ol_step_t step = ol_xfer_write(&s_xfer, (uint32_t)len);
    if (step != OL_STEP_OK) {
        fail(ol_step_text(step));
        ota_abort();
        return ESP_ERR_INVALID_SIZE;
    }

    const unsigned char *p = (const unsigned char *)data;
    size_t remaining = len;

    if (s_head_got < OL_HEADER_MIN) {
        size_t take = OL_HEADER_MIN - s_head_got;
        if (take > remaining) take = remaining;
        memcpy(s_head + s_head_got, p, take);
        s_head_got += take;
        p += take;
        remaining -= take;

        // Still not enough of the file to recognise it. The bytes are
        // counted but deliberately unwritten: nothing reaches the flash
        // before the verdict.
        if (s_head_got < OL_HEADER_MIN) return ESP_OK;

        const ol_image_verdict_t v = ol_check_image(s_head, s_head_got);
        if (v != OL_IMAGE_OK) {
            fail(ol_verdict_text(v));
            ota_abort();
            return ESP_ERR_INVALID_VERSION;
        }

        char version[OL_DESC_FIELD + 1] = {0};
        if (ol_image_version(s_head, s_head_got, version, sizeof(version))) {
            ESP_LOGW(TAG, "incoming image is version %s (running %s)",
                     version, ota_running_version());
        }

        const esp_err_t err = esp_ota_write(s_handle, s_head, s_head_got);
        if (err != ESP_OK) {
            fail(esp_err_to_name(err));
            ota_abort();
            return err;
        }
    }

    if (remaining == 0) return ESP_OK;

    const esp_err_t err = esp_ota_write(s_handle, p, remaining);
    if (err != ESP_OK) {
        fail(esp_err_to_name(err));
        ota_abort();
        return err;
    }
    return ESP_OK;
}

esp_err_t ota_end(void) {
    if (s_handle == 0) {
        fail("no update is running");
        return ESP_ERR_INVALID_STATE;
    }

    const ol_step_t step = ol_xfer_end(&s_xfer);
    if (step != OL_STEP_OK) {
        fail(ol_step_text(step));
        ota_abort();
        return ESP_ERR_INVALID_SIZE;
    }

    // Verifies the image, including the SHA-256 in its own last 32 bytes.
    esp_err_t err = esp_ota_end(s_handle);
    s_handle = 0;  // consumed either way; a second esp_ota_end would be a use-after-free
    if (err != ESP_OK) {
        fail(err == ESP_ERR_OTA_VALIDATE_FAILED ? "the image did not verify"
                                                : esp_err_to_name(err));
        ota_abort();
        return err;
    }

    err = esp_ota_set_boot_partition(s_target);
    if (err != ESP_OK) {
        fail(esp_err_to_name(err));
        ota_abort();
        return err;
    }

    ESP_LOGW(TAG, "update committed to %s; the next boot runs it unproven",
             s_target->label);
    s_target = NULL;
    s_head_got = 0;
    return ESP_OK;
}
