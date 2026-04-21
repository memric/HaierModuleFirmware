/*
 * ota.c
 *
 *  Created on: 1.12.2020
 *  Author: Valeriy Chudnikov
 */

#include "FWTP/fwtp.h"
#include "errno.h"
#include "esp_flash_partitions.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdint.h>
#include <string.h>

#define HASH_LEN 32 /* SHA-256 digest length */

static const char *TAG = "OTA";

static void print_sha256(const uint8_t *image_hash, const char *label);
static const esp_partition_t *update_partition = NULL;
static esp_ota_handle_t update_handle          = 0;
TimerHandle_t reboot_timer;

void RebootTimerCallback(TimerHandle_t xTimer);

uint32_t OTA_Init(void)
{
    uint8_t sha_256[HASH_LEN] = {0};
    esp_partition_t partition;

    // get sha256 digest for the partition table
    partition.address = ESP_PARTITION_TABLE_OFFSET;
    partition.size    = ESP_PARTITION_TABLE_MAX_LEN;
    partition.type    = ESP_PARTITION_TYPE_DATA;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for the partition table: ");

    // get sha256 digest for bootloader
    partition.address = ESP_BOOTLOADER_OFFSET;
    partition.size    = ESP_PARTITION_TABLE_OFFSET;
    partition.type    = ESP_PARTITION_TYPE_APP;
    esp_partition_get_sha256(&partition, sha_256);
    print_sha256(sha_256, "SHA-256 for bootloader: ");

    // get sha256 digest for running partition
    esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
    print_sha256(sha_256, "SHA-256 for current firmware: ");

    const esp_partition_t *configured = esp_ota_get_boot_partition();
    const esp_partition_t *running    = esp_ota_get_running_partition();

    if (configured != running)
    {
        ESP_LOGW(
            TAG, "Configured OTA boot partition at offset 0x%08" PRIx32 ", but running from offset 0x%08" PRIx32,
            configured->address, running->address
        );
        ESP_LOGW(
            TAG, "(This can happen if either the OTA boot data or preferred boot image become corrupted somehow.)"
        );
    }
    ESP_LOGI(
        TAG, "Running partition type %d subtype %d (offset 0x%08" PRIx32 ")", running->type, running->subtype,
        running->address
    );

    esp_ota_img_states_t ota_state;
    if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK)
    {
        if (ota_state == ESP_OTA_IMG_PENDING_VERIFY)
        {

            ESP_LOGW(TAG, "Running partition is need to be verified");
#if 1
            ESP_LOGI(TAG, "Firmware is verified");
            esp_ota_mark_app_valid_cancel_rollback();
#else
            ESP_LOGE(TAG, "Diagnostics failed! Start rollback to the previous version ...");
            esp_ota_mark_app_invalid_rollback_and_reboot();
#endif
        }
    }

    update_partition = esp_ota_get_next_update_partition(NULL);
    assert(update_partition != NULL);
    ESP_LOGI(
        TAG, "Writing to partition subtype %d at offset 0x%" PRIx32, update_partition->subtype,
        update_partition->address
    );

    /*Starting FWTP as s base transfer protocol*/
    FWTP_Init();

    return 0;
}

uint32_t OTA_Deinit(void)
{
    ESP_LOGI(TAG, "OTA deinitialization");

    FWTP_Deinit();

    return 0;
}

static void print_sha256(const uint8_t *image_hash, const char *label)
{
    char hash_print[HASH_LEN * 2 + 1];
    hash_print[HASH_LEN * 2] = 0;
    for (int i = 0; i < HASH_LEN; ++i)
    {
        sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
    }
    ESP_LOGI(TAG, "%s: %s", label, hash_print);
}

/**
 * @brief Writes file block to file system or memory corresponding to file_id
 * @param file_id
 * @param ttl_fsize
 * @param offset
 * @param len
 * @param p
 * @return
 */
uint32_t FWTP_BlockWrite(uint8_t file_id, uint32_t ttl_fsize, uint32_t offset, uint16_t len, const uint8_t *p)
{
    if (file_id != FWTP_MAINSYSTEM_FILE_ID)
    {
        return 0;
    }

    esp_err_t err;

    ESP_LOGI(TAG, "Block received; File ID: %u; Offset: %" PRIu32 "; Size: %" PRIu16 "", file_id, offset, len);

    (void) ttl_fsize;

    err = esp_ota_write_with_offset(update_handle, (const void *) p, len, offset);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Block writing error");
        return FWTP_ERR_FS;
    }

    return len;
}

/**
 * @brief File start command callback
 * @param file_id
 * @param ttl_fsize
 * @return
 */
uint32_t FWTP_FileStart(uint8_t file_id, uint32_t ttl_fsize)
{
    if (file_id != FWTP_MAINSYSTEM_FILE_ID)
    {
        return 0;
    }

    esp_err_t err;

    ESP_LOGI(TAG, "File start command received; File ID: %u; Size: %" PRIu32 "", file_id, ttl_fsize);

    if (file_id == FWTP_MAINSYSTEM_FILE_ID)
    {
        err = esp_ota_begin(update_partition, ttl_fsize, &update_handle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
            return FWTP_ERR_FS;
        }
        ESP_LOGI(TAG, "esp_ota_begin succeeded");
    }
    else
    {
        return FWTP_ERR_FILE_ID;
    }

    return FWTP_ERR_OK;
}

/**
 * @brief File stop command callback
 * @param file_id
 * @return
 */
uint32_t FWTP_FileStop(uint8_t file_id)
{
    if (file_id != FWTP_MAINSYSTEM_FILE_ID)
    {
        return 0;
    }

    esp_err_t err;

    ESP_LOGI(TAG, "File stop command received; File ID: %u", file_id);

    if (update_handle)
    {
        err = esp_ota_end(update_handle);
        if (err != ESP_OK)
        {
            if (err == ESP_ERR_OTA_VALIDATE_FAILED)
            {
                ESP_LOGE(TAG, "Image validation failed, image is corrupted");
            }
            ESP_LOGE(TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
            return FWTP_ERR_FS;
        }

        err = esp_ota_set_boot_partition(update_partition);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        }
    }

    ESP_LOGI(TAG, "Ready for system restart");

    reboot_timer = xTimerCreate("Reboot", 1000 / portTICK_PERIOD_MS, pdFALSE, (void *) 0, RebootTimerCallback);
    xTimerStart(reboot_timer, 0);

    return FWTP_ERR_OK;
}

void RebootTimerCallback(TimerHandle_t xTimer)
{
    esp_restart();
}