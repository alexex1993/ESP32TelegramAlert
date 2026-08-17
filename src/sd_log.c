#include "sd_log.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "app_config.h"
#include "display.h"
#include "settings.h"

static const char *TAG = "sdlog";

static sdmmc_card_t *s_card;
static bool s_mounted;

bool sd_log_mounted(void)
{
    return s_mounted;
}

bool sd_log_init(void)
{
    // SDSPI rides on the LCD's already-initialised SPI2 bus: esp_vfs_fat_sdspi_
    // mount() only attaches a device (spi_bus_add_device) and probes the card,
    // it never re-initialises the bus. The LCD left MISO unconfigured, so
    // display.c had to wire BOARD_SD_PIN_MISO into the bus config first.
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.max_freq_khz = BOARD_SD_FREQ_KHZ;

    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = BOARD_SD_SPI_HOST;
    slot.gpio_cs = BOARD_SD_PIN_CS;

    esp_vfs_fat_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    // Probing the card is dozens of SPI transactions, and the LVGL task has
    // been painting the screen over the same bus since display_init(). Every
    // card access on this bus runs under the panel's guard; the mount is no
    // exception -- see display_spi_bus_lock().
    display_spi_bus_lock();
    esp_err_t err = esp_vfs_fat_sdspi_mount(BOARD_SD_MOUNT_POINT, &host, &slot,
                                             &mount_cfg, &s_card);
    display_spi_bus_unlock();
    if (err != ESP_OK) {
        // No card inserted, or wrong format: not fatal. The pager pages and
        // receipts fine without the log; flag it and move on.
        ESP_LOGW(TAG, "SD mount failed (%s), logging disabled",
                 esp_err_to_name(err));
        s_card = NULL;
        s_mounted = false;
        return false;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD mounted, %" PRIu64 " MB capacity",
             ((uint64_t)s_card->csd.capacity) * s_card->csd.sector_size / (1024 * 1024));

    // Create the log root once; ignore "already exists". Per-chat and per-day
    // directories are created lazily by sd_log_message.
    display_spi_bus_lock();
    mkdir(SD_LOG_ROOT, 0777);
    display_spi_bus_unlock();

    return true;
}

// Breaks a Unix-seconds timestamp into local calendar fields, applying the same
// TZ offset the screen does so an on-card date matches what the pager displays.
// The offset is a runtime setting now, read through settings_get().
static void breakdown_local(time_t epoch, struct tm *out)
{
    time_t shifted = epoch + (time_t)settings_get()->tz_offset_hours * 3600;
    gmtime_r(&shifted, out);
}

// mkdir that treats "already exists" as success. FAT has no errno reliably
// across the VFS layer, so stat() is the portable existence check.
static void mkdir_p(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return;
    }
    if (mkdir(path, 0777) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "mkdir \"%s\" failed (errno %d)", path, errno);
    }
}

void sd_log_message(const pager_msg_t *msg)
{
    if (!s_mounted || msg == NULL) {
        return;
    }

    struct tm tm;
    breakdown_local((time_t)msg->date, &tm);

    // Path segments sized for GCC's pessimistic format-truncation analysis
    // (-Werror): it assumes each %d could fill a full int, so the buffers must
    // hold the literal worst case even though the real values are tiny. The
    // actual strings are short; snprintf still null-terminates regardless.
    char chat_dir[48];
    if (pager_msg_is_inline(msg)) {
        // A page sent inline belongs to no chat -- filing it under a chat id
        // of "0" would read like a bug in the tree rather than a fact about
        // how it arrived.
        snprintf(chat_dir, sizeof(chat_dir), SD_LOG_ROOT "/inline");
    } else {
        snprintf(chat_dir, sizeof(chat_dir), SD_LOG_ROOT "/%lld", (long long)msg->chat_id);
    }

    char day_dir[96];
    snprintf(day_dir, sizeof(day_dir), "%s/%04d-%02d-%02d", chat_dir,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);

    // From here on every line talks to the card, so the whole write is one
    // held stretch of the shared bus rather than a dozen: the panel's frame
    // flushes wait for it, and none of them can interleave into the middle of
    // a directory creation or a file. Nothing below may touch LVGL -- the
    // deadlock order is documented at display_spi_bus_lock().
    display_spi_bus_lock();

    // Build the directories lazily: a new chat or a new day must not lose its
    // first message to a missing parent.
    mkdir_p(chat_dir);
    mkdir_p(day_dir);

    // <HH-MM-SS>.txt as specified; two messages in the same chat within the same
    // second would otherwise silently overwrite, so on collision the message id
    // is appended -- unique and still self-describing.
    char path[160];
    snprintf(path, sizeof(path), "%s/%02d-%02d-%02d.txt", day_dir,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    struct stat st;
    if (stat(path, &st) == 0) {
        snprintf(path, sizeof(path), "%s/%02d-%02d-%02d_%lld.txt", day_dir,
                 tm.tm_hour, tm.tm_min, tm.tm_sec, (long long)msg->message_id);
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        ESP_LOGW(TAG, "open \"%s\" failed (errno %d)", path, errno);
        display_spi_bus_unlock();
        return;
    }

    // The header repeats what the directory and file name already encode, so a
    // file copied out of the tree is still self-describing. The body is the raw
    // UTF-8 text exactly as Telegram sent it.
    fprintf(f, "From: %s\n", msg->from);
    fprintf(f, "Date: %04d-%02d-%02d %02d:%02d:%02d UTC%+d\n",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec, (int)settings_get()->tz_offset_hours);
    if (pager_msg_is_inline(msg)) {
        fprintf(f, "Chat: inline\n");
        fprintf(f, "Message: %s\n", msg->inline_message_id);
    } else {
        fprintf(f, "Chat: %lld\n", (long long)msg->chat_id);
        fprintf(f, "Message: %lld\n", (long long)msg->message_id);
    }
    fprintf(f, "\n%s\n", msg->text);

    if (fclose(f) != 0) {
        ESP_LOGW(TAG, "close \"%s\" failed (errno %d)", path, errno);
    }

    display_spi_bus_unlock();
}
