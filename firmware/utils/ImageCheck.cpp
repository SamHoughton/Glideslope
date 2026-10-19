#include "utils/ImageCheck.h"
#include "utils/TelnetLogger.h"
#include <esp_ota_ops.h>

// The Arduino core marks a new image valid at startup unless this says to
// wait; ImageCheck::keep() does it once the image has proved itself.
extern "C" bool verifyRollbackLater() { return true; }

namespace
{
    bool s_pending = false;
}

void ImageCheck::begin()
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    s_pending = running && esp_ota_get_state_partition(running, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
    if (s_pending)
        Log.println("Firmware: new image on trial: kept after a good fetch or 5 minutes; a crash before then rolls back");
}

void ImageCheck::keep(const char *why)
{
    if (!s_pending) return;
    s_pending = false;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        Log.printf("Firmware: new image kept (%s)\n", why);
}

const char *ImageCheck::state()
{
    if (s_pending) return "on trial";
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!running || esp_ota_get_state_partition(running, &st) != ESP_OK) return "factory";
    return "kept";
}
