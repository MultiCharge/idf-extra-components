/*
 * SPDX-FileCopyrightText: 2015-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/param.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "usb/usb_host.h"
#include "diskio_usb.h"
#include "msc_common.h"
#include "usb/msc_host.h"
#include "msc_scsi_bot.h"
#include "usb/usb_types_ch9.h"
#include "usb/usb_helpers.h"
#include "soc/soc_memory_layout.h"

static portMUX_TYPE msc_lock = portMUX_INITIALIZER_UNLOCKED;

#define MSC_ENTER_CRITICAL()    portENTER_CRITICAL(&msc_lock)
#define MSC_EXIT_CRITICAL()     portEXIT_CRITICAL(&msc_lock)

#define MSC_GOTO_ON_FALSE_CRITICAL(exp, err)    \
    do {                                        \
        if(!(exp)) {                            \
            MSC_EXIT_CRITICAL();                \
            ret = err;                          \
            goto fail;                          \
        }                                       \
    } while(0)

#define MSC_RETURN_ON_FALSE_CRITICAL(exp, err)  \
    do {                                        \
        if(!(exp)) {                            \
            MSC_EXIT_CRITICAL();                \
            return err;                         \
        }                                       \
    } while(0)

#define DEFAULT_XFER_SIZE   (64) // Transfer size used for all transfers apart from SCSI read/write
#define WAIT_FOR_READY_TIMEOUT_MS 3000
#define SCSI_COMMAND_SET    0x06
#define BULK_ONLY_TRANSFER  0x50
#define MSC_NO_SENSE        0x00
#define MSC_NOT_READY       0x02
#define MSC_UNIT_ATTENTION  0x06

static const char *TAG = "USB_MSC";
typedef struct {
    usb_host_client_handle_t client_handle;
    msc_host_event_cb_t user_cb;
    void *user_arg;
    SemaphoreHandle_t all_events_handled;
    volatile bool end_client_event_handling;
    STAILQ_HEAD(devices, msc_host_device) devices_tailq;
} msc_driver_t;

static msc_driver_t *s_msc_driver;


static const usb_standard_desc_t *next_interface_desc(const usb_standard_desc_t *desc, size_t len, size_t *offset)
{
    return usb_parse_next_descriptor_of_type(desc, len, USB_W_VALUE_DT_INTERFACE, (int *)offset);
}

static const usb_standard_desc_t *next_endpoint_desc(const usb_standard_desc_t *desc, size_t len, size_t *offset)
{
    return usb_parse_next_descriptor_of_type(desc, len, USB_B_DESCRIPTOR_TYPE_ENDPOINT, (int *)offset);
}

static inline bool is_in_endpoint(uint8_t endpoint)
{
    return endpoint & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK ? true : false;
}

static const usb_intf_desc_t *find_msc_interface(const usb_config_desc_t *config_desc, size_t *offset)
{
    size_t total_length = config_desc->wTotalLength;
    const usb_standard_desc_t *next_desc = (const usb_standard_desc_t *)config_desc;

    next_desc = next_interface_desc(next_desc, total_length, offset);

    while ( next_desc ) {

        const usb_intf_desc_t *ifc_desc = (const usb_intf_desc_t *)next_desc;

        if ( ifc_desc->bInterfaceClass == USB_CLASS_MASS_STORAGE &&
                ifc_desc->bInterfaceSubClass == SCSI_COMMAND_SET &&
                ifc_desc->bInterfaceProtocol == BULK_ONLY_TRANSFER ) {
            return ifc_desc;
        }

        next_desc = next_interface_desc(next_desc, total_length, offset);
    };
    return NULL;
}

/**
 * @brief Extracts configuration from configuration descriptor.
 *
 * @note  Passes interface and endpoint descriptors to obtain:

 *        - interface number, IN endpoint, OUT endpoint, max. packet size
 *
 * @param[in]  cfg_desc  Configuration descriptor
 * @param[out] cfg       Obtained configuration
 * @return esp_err_t
 */
static esp_err_t extract_config_from_descriptor(const usb_config_desc_t *cfg_desc, msc_config_t *cfg)
{
    size_t offset = 0;
    size_t total_len = cfg_desc->wTotalLength;
    const usb_intf_desc_t *ifc_desc = find_msc_interface(cfg_desc, &offset);
    assert(ifc_desc);
    const usb_standard_desc_t *next_desc = (const usb_standard_desc_t *)ifc_desc;
    const usb_ep_desc_t *ep_desc = NULL;

    cfg->iface_num = ifc_desc->bInterfaceNumber;

    next_desc = next_endpoint_desc(next_desc, total_len, &offset);
    MSC_RETURN_ON_FALSE(next_desc, ESP_ERR_NOT_SUPPORTED);
    ep_desc = (const usb_ep_desc_t *)next_desc;

    if (is_in_endpoint(ep_desc->bEndpointAddress)) {
        cfg->bulk_in_ep = ep_desc->bEndpointAddress;
        cfg->bulk_in_mps = ep_desc->wMaxPacketSize;
    } else {
        cfg->bulk_out_ep = ep_desc->bEndpointAddress;
    }

    next_desc = next_endpoint_desc(next_desc, total_len, &offset);
    MSC_RETURN_ON_FALSE(next_desc, ESP_ERR_NOT_SUPPORTED);
    ep_desc = (const usb_ep_desc_t *)next_desc;

    if (is_in_endpoint(ep_desc->bEndpointAddress)) {
        cfg->bulk_in_ep = ep_desc->bEndpointAddress;
        cfg->bulk_in_mps = ep_desc->wMaxPacketSize;
    } else {
        cfg->bulk_out_ep = ep_desc->bEndpointAddress;
    }

    return ESP_OK;
}

static esp_err_t msc_deinit_device(msc_device_t *dev, bool install_failed)
{
    MSC_ENTER_CRITICAL();
    MSC_RETURN_ON_FALSE_CRITICAL( dev, ESP_ERR_INVALID_STATE );
    STAILQ_REMOVE(&s_msc_driver->devices_tailq, dev, msc_host_device, tailq_entry);
    MSC_EXIT_CRITICAL();

    if (dev->transfer_done) {
        vSemaphoreDelete(dev->transfer_done);
    }
    if (install_failed) {
        // Error code is unchecked, as it's unknown at what point installation failed.
        usb_host_interface_release(s_msc_driver->client_handle, dev->handle, dev->config.iface_num);
        usb_host_device_close(s_msc_driver->client_handle, dev->handle);
        usb_host_transfer_free(dev->xfer);
    } else {
        MSC_RETURN_ON_ERROR( usb_host_interface_release(s_msc_driver->client_handle, dev->handle, dev->config.iface_num) );
        MSC_RETURN_ON_ERROR( usb_host_device_close(s_msc_driver->client_handle, dev->handle) );
        MSC_RETURN_ON_ERROR( usb_host_transfer_free(dev->xfer) );
    }

    free(dev);
    return ESP_OK;
}

// Some MSC devices requires to change its internal state from non-ready to ready
static esp_err_t msc_wait_for_ready_state(msc_device_t *dev, size_t timeout_ms)
{
    esp_err_t err;
    scsi_sense_data_t sense;
    uint32_t trials = MAX(1, timeout_ms / 100);

    do {
        err = scsi_cmd_unit_ready(dev);
        if (err != ESP_OK) {
            MSC_RETURN_ON_ERROR( scsi_cmd_sense(dev, &sense) );
            if (sense.key != MSC_NOT_READY &&
                    sense.key != MSC_UNIT_ATTENTION &&
                    sense.key != MSC_NO_SENSE) {
                return ESP_ERR_MSC_INTERNAL;
            }
        }
        vTaskDelay( pdMS_TO_TICKS(100) );
    } while (trials-- && err);

    return err;
}

static bool is_mass_storage_device(uint8_t dev_addr)
{
    size_t dummy = 0;
    bool is_msc_device = false;
    usb_device_handle_t device;
    const usb_config_desc_t *config_desc;

    if ( usb_host_device_open(s_msc_driver->client_handle, dev_addr, &device) == ESP_OK) {
        if ( usb_host_get_active_config_descriptor(device, &config_desc) == ESP_OK ) {
            if ( find_msc_interface(config_desc, &dummy) ) {
                is_msc_device = true;
            } else {
                ESP_LOGD(TAG, "Connected USB device is not MSC");
            }
        }
        usb_host_device_close(s_msc_driver->client_handle, device);
    }

    return is_msc_device;
}

static void event_handler_task(void *arg)
{
    while (1) {
        usb_host_client_handle_events(s_msc_driver->client_handle, portMAX_DELAY);

        if (s_msc_driver->end_client_event_handling) {
            break;
        }
    }
    ESP_ERROR_CHECK( usb_host_client_deregister(s_msc_driver->client_handle) );
    xSemaphoreGive(s_msc_driver->all_events_handled);
    vTaskDelete(NULL);
}

static msc_device_t *find_msc_device(usb_device_handle_t device_handle)
{
    msc_host_device_handle_t device;

    STAILQ_FOREACH(device, &s_msc_driver->devices_tailq, tailq_entry) {
        if (device_handle == device->handle) {
            return device;
        }
    }

    return NULL;
}

static void client_event_cb(const usb_host_client_event_msg_t *event, void *arg)
{
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        if (is_mass_storage_device(event->new_dev.address)) {
            const msc_host_event_t msc_event = {
                .event = MSC_DEVICE_CONNECTED,
                .device.address = event->new_dev.address,
            };
            s_msc_driver->user_cb(&msc_event, s_msc_driver->user_arg);
        }
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        msc_device_t *msc_device = find_msc_device(event->dev_gone.dev_hdl);
        if (msc_device) {
            const msc_host_event_t msc_event = {
                .event = MSC_DEVICE_DISCONNECTED,
                .device.handle = msc_device,
            };
            s_msc_driver->user_cb(&msc_event, s_msc_driver->user_arg);
        }
    }
}

esp_err_t msc_host_install(const msc_host_driver_config_t *config)
{
    esp_err_t ret;

    MSC_RETURN_ON_INVALID_ARG(config);
    MSC_RETURN_ON_INVALID_ARG(config->callback);
    if ( config->create_backround_task ) {
        MSC_RETURN_ON_FALSE(config->stack_size != 0, ESP_ERR_INVALID_ARG);
        MSC_RETURN_ON_FALSE(config->task_priority != 0, ESP_ERR_INVALID_ARG);
    }
    MSC_RETURN_ON_FALSE(!s_msc_driver, ESP_ERR_INVALID_STATE);

    msc_driver_t *driver = calloc(1, sizeof(msc_driver_t));
    MSC_RETURN_ON_FALSE(driver, ESP_ERR_NO_MEM);
    driver->user_cb = config->callback;
    driver->user_arg = config->callback_arg;

    usb_host_client_config_t client_config = {
        .async.client_event_callback = client_event_cb,
        .async.callback_arg = NULL,
        .max_num_event_msg = 10,
    };

    driver->end_client_event_handling = false;
    driver->all_events_handled = xSemaphoreCreateBinary();
    MSC_GOTO_ON_FALSE(driver->all_events_handled, ESP_ERR_NO_MEM);

    MSC_GOTO_ON_ERROR( usb_host_client_register(&client_config, &driver->client_handle) );

    MSC_ENTER_CRITICAL();
    MSC_GOTO_ON_FALSE_CRITICAL(!s_msc_driver, ESP_ERR_INVALID_STATE);
    s_msc_driver = driver;
    STAILQ_INIT(&s_msc_driver->devices_tailq);
    MSC_EXIT_CRITICAL();

    if (config->create_backround_task) {
        BaseType_t task_created = xTaskCreatePinnedToCore(
                                      event_handler_task,
                                      "USB MSC",
                                      config->stack_size,
                                      NULL,
                                      config->task_priority,
                                      NULL,
                                      config->core_id);
        MSC_GOTO_ON_FALSE(task_created, ESP_ERR_NO_MEM);
    }

    return ESP_OK;

fail:
    s_msc_driver = NULL;
    usb_host_client_deregister(driver->client_handle);
    if (driver->all_events_handled) {
        vSemaphoreDelete(driver->all_events_handled);
    }
    free(driver);
    return ret;
}

esp_err_t msc_host_uninstall(void)
{
    // Make sure msc driver is installed,
    // not being uninstalled from other task
    // and no msc device is registered
    MSC_ENTER_CRITICAL();
    MSC_RETURN_ON_FALSE_CRITICAL( s_msc_driver != NULL, ESP_ERR_INVALID_STATE );
    MSC_RETURN_ON_FALSE_CRITICAL( !s_msc_driver->end_client_event_handling, ESP_ERR_INVALID_STATE );
    MSC_RETURN_ON_FALSE_CRITICAL( STAILQ_EMPTY(&s_msc_driver->devices_tailq), ESP_ERR_INVALID_STATE );
    s_msc_driver->end_client_event_handling = true;
    MSC_EXIT_CRITICAL();

    ESP_ERROR_CHECK( usb_host_client_unblock(s_msc_driver->client_handle) );
    xSemaphoreTake(s_msc_driver->all_events_handled, portMAX_DELAY);
    vSemaphoreDelete(s_msc_driver->all_events_handled);
    free(s_msc_driver);
    s_msc_driver = NULL;
    return ESP_OK;
}

esp_err_t msc_host_install_device(uint8_t device_address, msc_host_device_handle_t *msc_device_handle)
{
    esp_err_t ret;
    uint32_t block_size, block_count;
    const usb_config_desc_t *config_desc;
    msc_device_t *msc_device;

    MSC_GOTO_ON_FALSE( msc_device = calloc(1, sizeof(msc_device_t)), ESP_ERR_NO_MEM );

    MSC_ENTER_CRITICAL();
    MSC_GOTO_ON_FALSE_CRITICAL( s_msc_driver, ESP_ERR_INVALID_STATE );
    MSC_GOTO_ON_FALSE_CRITICAL( s_msc_driver->client_handle, ESP_ERR_INVALID_STATE );
    STAILQ_INSERT_TAIL(&s_msc_driver->devices_tailq, msc_device, tailq_entry);
    MSC_EXIT_CRITICAL();

    MSC_GOTO_ON_FALSE( msc_device->transfer_done = xSemaphoreCreateBinary(), ESP_ERR_NO_MEM);
    MSC_GOTO_ON_ERROR( usb_host_device_open(s_msc_driver->client_handle, device_address, &msc_device->handle) );
    MSC_GOTO_ON_ERROR( usb_host_get_active_config_descriptor(msc_device->handle, &config_desc) );
    MSC_GOTO_ON_ERROR( extract_config_from_descriptor(config_desc, &msc_device->config) );
    MSC_GOTO_ON_ERROR( usb_host_transfer_alloc(DEFAULT_XFER_SIZE, 0, &msc_device->xfer) );
    MSC_GOTO_ON_ERROR( usb_host_interface_claim(
                           s_msc_driver->client_handle,
                           msc_device->handle,
                           msc_device->config.iface_num, 0) );

    MSC_GOTO_ON_ERROR( scsi_cmd_inquiry(msc_device) );
    MSC_GOTO_ON_ERROR( msc_wait_for_ready_state(msc_device, WAIT_FOR_READY_TIMEOUT_MS) );
    MSC_GOTO_ON_ERROR( scsi_cmd_read_capacity(msc_device, &block_size, &block_count) );

    msc_device->disk.block_size = block_size;
    msc_device->disk.block_count = block_count;
    *msc_device_handle = msc_device;

    return ESP_OK;

fail:
    msc_deinit_device(msc_device, true);
    return ret;
}

esp_err_t msc_host_uninstall_device(msc_host_device_handle_t device)
{
    MSC_RETURN_ON_INVALID_ARG(device);
    return msc_deinit_device((msc_device_t *)device, false);
}

esp_err_t msc_host_handle_events(uint32_t timeout_ms)
{
    MSC_RETURN_ON_FALSE(s_msc_driver != NULL, ESP_ERR_INVALID_STATE);

    return usb_host_client_handle_events(s_msc_driver->client_handle, pdMS_TO_TICKS(timeout_ms));
}

esp_err_t msc_host_get_device_info(msc_host_device_handle_t device, msc_host_device_info_t *info)
{
    MSC_RETURN_ON_INVALID_ARG(device);
    MSC_RETURN_ON_INVALID_ARG(info);

    msc_device_t *dev = (msc_device_t *)device;
    const usb_device_desc_t *desc;
    usb_device_info_t dev_info;

    MSC_RETURN_ON_ERROR( usb_host_get_device_descriptor(dev->handle, &desc) );
    MSC_RETURN_ON_ERROR( usb_host_device_info(dev->handle, &dev_info) );

    info->idProduct = desc->idProduct;
    info->idVendor = desc->idVendor;
    info->sector_size = dev->disk.block_size;
    info->sector_count = dev->disk.block_count;

    size_t len = MIN((dev_info.str_desc_manufacturer->bLength - USB_STANDARD_DESC_SIZE) / 2, MSC_STR_DESC_SIZE - 1);
    wcsncpy(info->iManufacturer, dev_info.str_desc_manufacturer->wData, len);
    info->iManufacturer[len] = 0;

    len = MIN((dev_info.str_desc_product->bLength - USB_STANDARD_DESC_SIZE) / 2, MSC_STR_DESC_SIZE - 1);
    wcsncpy(info->iProduct, dev_info.str_desc_product->wData, len);
    info->iProduct[len] = 0;

    len = MIN((dev_info.str_desc_serial_num->bLength - USB_STANDARD_DESC_SIZE) / 2, MSC_STR_DESC_SIZE - 1);
    wcsncpy(info->iSerialNumber, dev_info.str_desc_serial_num->wData, len);
    info->iSerialNumber[len] = 0;

    return ESP_OK;
}

esp_err_t msc_host_print_descriptors(msc_host_device_handle_t device)
{
    msc_device_t *dev = (msc_device_t *)device;
    const usb_device_desc_t *device_desc;
    const usb_config_desc_t *config_desc;
    MSC_RETURN_ON_ERROR( usb_host_get_device_descriptor(dev->handle, &device_desc) );
    MSC_RETURN_ON_ERROR( usb_host_get_active_config_descriptor(dev->handle, &config_desc) );
    usb_print_device_descriptor(device_desc);
    usb_print_config_descriptor(config_desc, NULL);
    return ESP_OK;
}

static void transfer_callback(usb_transfer_t *transfer)
{
    msc_device_t *device = (msc_device_t *)transfer->context;

    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGE("Transfer failed", "Status %d", transfer->status);
    }

    xSemaphoreGive(device->transfer_done);
}

static usb_transfer_status_t wait_for_transfer_done(usb_transfer_t *xfer)
{
    msc_device_t *device = (msc_device_t *)xfer->context;
    usb_transfer_status_t status = xfer->status;
    BaseType_t received = xSemaphoreTake(device->transfer_done, pdMS_TO_TICKS(xfer->timeout_ms));

    if (received != pdTRUE) {
        usb_host_endpoint_halt(xfer->device_handle, xfer->bEndpointAddress);
        usb_host_endpoint_flush(xfer->device_handle, xfer->bEndpointAddress);
        xSemaphoreTake(device->transfer_done, portMAX_DELAY);
        return USB_TRANSFER_STATUS_TIMED_OUT;
    }

    return status;
}

esp_err_t msc_bulk_transfer(msc_device_t *device, uint8_t *data, size_t size, msc_endpoint_t ep)
{
    usb_transfer_t *xfer = device->xfer;
    MSC_RETURN_ON_FALSE(size <= xfer->data_buffer_size, ESP_ERR_INVALID_SIZE);
    uint8_t endpoint = (ep == MSC_EP_IN) ? device->config.bulk_in_ep : device->config.bulk_out_ep;

    if (is_in_endpoint(endpoint)) {
        xfer->num_bytes = usb_round_up_to_mps(size, device->config.bulk_in_mps);
    } else {
        memcpy(xfer->data_buffer, data, size);
        xfer->num_bytes = size;
    }

    xfer->device_handle = device->handle;
    xfer->bEndpointAddress = endpoint;
    xfer->callback = transfer_callback;
    xfer->timeout_ms = 5000;
    xfer->context = device;

    MSC_RETURN_ON_ERROR( usb_host_transfer_submit(xfer) );
    if (USB_TRANSFER_STATUS_COMPLETED != wait_for_transfer_done(xfer)) {
        if (USB_TRANSFER_STATUS_STALL == wait_for_transfer_done(xfer)) {
            return ESP_ERR_MSC_STALL;
        }
        return ESP_ERR_MSC_INTERNAL;
    }

    if (is_in_endpoint(endpoint)) {
        memcpy(data, xfer->data_buffer, size);
    }

    return ESP_OK;
}

esp_err_t msc_bulk_transfer_zero_cpy(msc_device_t *device, uint8_t *data, size_t size, msc_endpoint_t ep)
{
    esp_err_t ret = ESP_OK;
    MSC_RETURN_ON_FALSE(esp_ptr_dma_capable(data), ESP_FAIL); // The passed buffer must be DMA capable
    usb_transfer_t *xfer = device->xfer;
    uint8_t endpoint = (ep == MSC_EP_IN) ? device->config.bulk_in_ep : device->config.bulk_out_ep;
    uint8_t *backup_buffer = xfer->data_buffer;
    size_t backup_size = xfer->data_buffer_size;
    size_t actual_size;

    uint8_t **ptr = (uint8_t **)(&(xfer->data_buffer));
    size_t *siz = (size_t *)(&(xfer->data_buffer_size));

    if (is_in_endpoint(endpoint)) {
        actual_size = usb_round_up_to_mps(size, device->config.bulk_in_mps);
    } else {
        actual_size = size;
    }

    // Attention: Here we modify 'private' members data_buffer and data_buffer_size
    *ptr = data;
    *siz = actual_size;
    xfer->num_bytes = actual_size;
    xfer->device_handle = device->handle;
    xfer->bEndpointAddress = endpoint;
    xfer->callback = transfer_callback;
    xfer->timeout_ms = 5000;
    xfer->context = device;

    MSC_GOTO_ON_ERROR( usb_host_transfer_submit(xfer) );
    if (USB_TRANSFER_STATUS_COMPLETED != wait_for_transfer_done(xfer)) {
        MSC_GOTO_ON_FALSE(!(USB_TRANSFER_STATUS_STALL == wait_for_transfer_done(xfer)), ESP_ERR_MSC_STALL);
        ret = ESP_ERR_MSC_INTERNAL;
    }
fail:
    *ptr = backup_buffer;
    *siz = backup_size;
    return ret;
}

esp_err_t msc_control_transfer(msc_device_t *device, size_t len)
{
    usb_transfer_t *xfer = device->xfer;
    xfer->device_handle = device->handle;
    xfer->bEndpointAddress = 0;
    xfer->callback = transfer_callback;
    xfer->timeout_ms = 5000;
    xfer->num_bytes = len;
    xfer->context = device;

    MSC_RETURN_ON_ERROR( usb_host_transfer_submit_control(s_msc_driver->client_handle, xfer));
    return wait_for_transfer_done(xfer) == USB_TRANSFER_STATUS_COMPLETED ? ESP_OK : ESP_ERR_MSC_INTERNAL;
}
