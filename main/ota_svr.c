/* general and RTOS stuff */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
/* OTA partition stuff */
#include "esp_partition.h"
#include "esp_ota_ops.h"
/* Nimble Stuff */
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "bleprph.h"
#include "services/ans/ble_svc_ans.h"

/*** Maximum number of characteristics with the notify flag ***/
#define MAX_NOTIFY 5

/* A primary service - OTA service */
static const ble_uuid128_t gatt_ota_svc_uuid =
    BLE_UUID128_INIT(0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                     0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10);

/* OTA firmware transmission characteristic */
static uint16_t gatt_ota_firmware_chr_val_handle;
static const ble_uuid128_t gatt_ota_firmware_chr_uuid =
    BLE_UUID128_INIT(0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
                     0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01);

/* OTA firmware commands characteristic */
static uint16_t gatt_ota_command_chr_val_handle;
static const ble_uuid128_t gatt_ota_command_chr_uuid =
    BLE_UUID128_INIT(0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
                     0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10);

static int gatt_ota_firmware_callback(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);

static int gatt_ota_command_callback(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);

static RingbufHandle_t gatt_ota_ring_buffer_handle = NULL;
TaskHandle_t ota_task_handle = NULL;
esp_ota_handle_t ota_update_handle = 0;
void ota_task(void *arg);
uint32_t firmware_size = 0;

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        /*** OTA Service ***/
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &gatt_ota_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    /*** This characteristic can be subscribed to by writing 0x00 and 0x01 to the CCCD ***/
                    .uuid = &gatt_ota_command_chr_uuid.u,
                    .access_cb = gatt_ota_command_callback,
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_INDICATE,
                    .val_handle = &gatt_ota_command_chr_val_handle,
                },
                {
                    /*** OTA data characteristic ***/
                    .uuid = &gatt_ota_firmware_chr_uuid.u,
                    .access_cb = gatt_ota_firmware_callback,
                    .flags = BLE_GATT_CHR_F_WRITE,
                    .val_handle = &gatt_ota_firmware_chr_val_handle,
                },
                {
                    0, /* No more characteristics in this service. */
                }},
    },
    {
        0, /* No more services. */
    },
};

/* This is the callback function for the OTA data characteristic - The data that is received is sent to the ring buffer, on reception */
static int gatt_ota_firmware_callback(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op)
    {
    /* This is a write to the server - This is where we will receive OTA data */
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        /* Check if the OTA task is running */
        if (ota_task_handle != NULL)
        {
            struct os_mbuf *om = ctxt->om;
            while (om)
            {
                if (om->om_len > 0)
                    /* If the data has finite length, then send it to the ring buffer */
                    if (xRingbufferSend(gatt_ota_ring_buffer_handle, om->om_data, om->om_len, 0) != pdTRUE)
                    {
                        /* If the ring buffer is full, drop the data and throw error to signal to client that there is an error */
                        ESP_LOGW("OTA_SERVER", "Ring buffer might be full. Dropping data packet");
                        return BLE_ATT_ERR_INSUFFICIENT_RES;
                    }
                om = SLIST_NEXT(om, om_next);
            }
        }
        return 0;

    /* This is a read operation on the server - We don't need this. No access to read anything on this characteristic */
    default:
        ESP_LOGE("OTA_SERVER", "Attempted action which is not supported: %d", ctxt->op);
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/* This is the callback function for the OTA command characteristic - This is where the OTA size and initialization command is received */
static int gatt_ota_command_callback(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op)
    {
    /* This is a read operation on the server - We don't need this. No access to read anything on this characteristic */
    default:
        return BLE_ATT_ERR_UNLIKELY;

    /* This is a write to the server - This is where we will receive OTA data */
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        switch (ctxt->om->om_len)
        {
        /* Two bytes of data is the OTA initialization command - specifically it is 0x2025 */
        case 2:
            if (ctxt->om->om_data[0] == 0x20 && ctxt->om->om_data[1] == 0x25)
            {
                ESP_LOGI("OTA_SERVER", "OTA initialization command issued");
                if (ota_task_handle == NULL)
                    xTaskCreate(ota_task, "ota_task", 8192, NULL, 5, &ota_task_handle);
            }
            return 0;

        /* Four bytes of data is the OTA firmware size - standard packing size for this codebase */
        case 4:
            firmware_size = ctxt->om->om_data[0] | (ctxt->om->om_data[1] << 8) | (ctxt->om->om_data[2] << 16) | (ctxt->om->om_data[3] << 24);
            ESP_LOGI("OTA_SERVER", "OTA firmware size: %d", firmware_size);
            return 0;

        default:
            return 0;
        }
    }
}

void gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    char buf[BLE_UUID_STR_LEN];

    switch (ctxt->op)
    {
    case BLE_GATT_REGISTER_OP_SVC:
        MODLOG_DFLT(DEBUG, "registered service %s with handle=%d\n",
                    ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf),
                    ctxt->svc.handle);
        break;

    case BLE_GATT_REGISTER_OP_CHR:
        MODLOG_DFLT(DEBUG, "registering characteristic %s with "
                           "def_handle=%d val_handle=%d\n",
                    ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                    ctxt->chr.def_handle,
                    ctxt->chr.val_handle);
        break;

    case BLE_GATT_REGISTER_OP_DSC:
        MODLOG_DFLT(DEBUG, "registering descriptor %s with handle=%d\n",
                    ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf),
                    ctxt->dsc.handle);
        break;

    default:
        assert(0);
        break;
    }
}

int gatt_svr_init(void)
{
    int rc;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_ans_init();

    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0)
    {
        return rc;
    }

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0)
    {
        return rc;
    }

    return 0;
}

#define OTA_WRITE_BUFFER_SIZE 4096

void ota_task(void *arg)
{
    /* Only proceed further if the firmware size has been set before the task starts */
    if (firmware_size == 0)
    {
        ESP_LOGE("OTA_SERVER", "Firmware size not set before OTA data!");
        vTaskDelete(NULL);
    }
    ESP_LOGI("OTA_SERVER", "Ring buffer task created");

    gatt_ota_ring_buffer_handle = xRingbufferCreate((16 * 1024), RINGBUF_TYPE_BYTEBUF);
    if (gatt_ota_ring_buffer_handle == NULL)
    {
        ESP_LOGE("OTA_SERVER", "Failed to create ring buffer");
        return;
    }
    ESP_LOGI("OTA_SERVER", "Ring buffer created successfully");

    /* Get the current running partition and the next update partition - it is important to get the current and next parition because the firmware has to be written to the next partition. Overwriting in place will lead to memory/firmware corruption */
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    if (running_partition == NULL)
    {
        ESP_LOGE("OTA_SERVER", "Failed to find running partition");
        ota_task_handle = NULL;
        vTaskDelete(NULL);
    }
    const esp_partition_t *update_partition = esp_ota_get_next_update_partition(running_partition);
    if (update_partition == NULL)
    {
        ESP_LOGE("OTA_SERVER", "Failed to find update partition");
        ota_task_handle = NULL;
        vTaskDelete(NULL);
    }

    /* Begin OTA and tie it to the appropriate partition */
    esp_err_t ret = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_update_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE("OTA_SERVER", "Failed to begin OTA. Reason: %s", esp_err_to_name(ret));
        ota_task_handle = NULL;
        vTaskDelete(NULL);
    }
    ESP_LOGI("OTA_SERVER", "OTA started. Waiting for data. Expected firmware size: %d", firmware_size);

    uint8_t *received_data = NULL;
    size_t received_data_size = 0;
    uint32_t net_received_data_length = 0;
    uint8_t net_percentage = 0;

    uint8_t ota_write_buf[OTA_WRITE_BUFFER_SIZE];
    size_t buffer_offset = 0;

    /* Continuously receive data from the ring buffer */
    while (1)
    {
        received_data = (uint8_t *)xRingbufferReceive(gatt_ota_ring_buffer_handle, &received_data_size, portMAX_DELAY);
        if (received_data && received_data_size > 0)
        {
            /* Write data to OTA buffer - will be filled with data until it reaches OTA_WRITE_BUFFER_SIZE */
            if (buffer_offset + received_data_size <= OTA_WRITE_BUFFER_SIZE)
            {
                memcpy(ota_write_buf + buffer_offset, received_data, received_data_size);
                buffer_offset += received_data_size;
            }
            else
            {
                /* Write the entire buffer to the flash. This is done as opposed to writing every element from buffer because the writing time to the flash might become a choking point if accessed every turn. So, chunks of data are written at once instead of every element */
                ret = esp_ota_write(ota_update_handle, ota_write_buf, buffer_offset);
                if (ret != ESP_OK)
                {
                    /* If failed, return the data back to the ring buffer and delete the task */
                    vRingbufferReturnItem(gatt_ota_ring_buffer_handle, received_data);
                    ESP_LOGE("OTA_SERVER", "Failed to write OTA data. Data returned to ring buffer, deleting task. Reason: %s", esp_err_to_name(ret));
                    ota_task_handle = NULL;
                    vTaskDelete(NULL);
                }
                buffer_offset = 0;

                /* Copy the current iterm to the buffer, post writing the previous contents of the buffer into the flash */
                memcpy(ota_write_buf, received_data, received_data_size);
                buffer_offset = received_data_size;
            }
            /* Return item back to ensure ring buffer doesn't overflow */
            vRingbufferReturnItem(gatt_ota_ring_buffer_handle, received_data);

            /* Update metrics */
            net_received_data_length += received_data_size;
            int current_percentage = (net_received_data_length * 100) / firmware_size;
            if (current_percentage >= net_percentage + 10)
            {
                ESP_LOGI("OTA_SERVER", "OTA progress: %d%", current_percentage);
                net_percentage = current_percentage;
            }
            /* Check if OTA data reception is completed */
            if (firmware_size > 0 && net_received_data_length >= firmware_size)
            {
                ESP_LOGI("OTA_SERVER", "OTA data reception completed");
                break;
            }
        }
    }

    /* Flush any remaining data into the flash - usually there will be no data. But, if there is, write it to the flash */
    if (buffer_offset > 0)
    {
        esp_err_t ret = esp_ota_write(ota_update_handle, ota_write_buf, buffer_offset);
        if (ret != ESP_OK)
        {
            ESP_LOGE("OTA_SERVER", "Failed to write OTA data. Reason: %s", esp_err_to_name(ret));
            ota_task_handle = NULL;
            vTaskDelete(NULL);
        }
    }
    /* With all data cleared, end the OTA */
    ret = esp_ota_end(ota_update_handle);
    if (ret != ESP_OK)
    {
        ESP_LOGE("OTA_SERVER", "Failed to end OTA. Reason: %s", esp_err_to_name(ret));
        ota_task_handle = NULL;
        vTaskDelete(NULL);
    }
    /* Update the partition from which the ESP32 will boot and restart to boot up with the new firmware */
    ret = esp_ota_set_boot_partition(update_partition);
    if (ret != ESP_OK)
    {
        ESP_LOGE("OTA_SERVER", "Failed to set boot partition. Reason: %s", esp_err_to_name(ret));
        ota_task_handle = NULL;
        vTaskDelete(NULL);
    }

    ESP_LOGI("OTA_SERVER", "OTA update completed, rebooting device!");
    esp_restart();
}