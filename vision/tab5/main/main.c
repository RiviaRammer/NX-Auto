#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "bsp/esp-bsp.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"

/* Start conservatively. After checking the card's advertised modes on a PC,
 * try 640x480@15. Uncompressed YUY2 is intentionally not used here. */
#define CAPTURE_WIDTH             320
#define CAPTURE_HEIGHT            240
#define CAPTURE_FPS               15
#define MJPEG_FRAME_BYTES         (128 * 1024)
#define UVC_FRAME_BUFFERS         3
#define UVC_URBS                  16
#define UVC_URB_BYTES             (16 * 1024)

#define USB_TASK_PRIORITY         15
#define UVC_TASK_PRIORITY         16
#define DECODE_TASK_PRIORITY      5

static const char *TAG = "tab5_ns_uvc";
static QueueHandle_t frame_queue;
static uvc_host_stream_hdl_t stream_handle;
static volatile bool stream_running;

static bool frame_callback(const uvc_host_frame_t *frame, void *context)
{
    (void)context;
    /* Returning false transfers this frame to the application. If the latest
     * frame is still being decoded, drop the new frame to keep latency low. */
    if (xQueueSend(frame_queue, &frame, 0) == pdPASS) {
        return false;
    }
    return true;
}

static void stream_event_callback(const uvc_host_stream_event_data_t *event,
                                  void *context)
{
    (void)context;
    switch (event->type) {
    case UVC_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "采集卡已断开");
        stream_running = false;
        break;
    case UVC_HOST_FRAME_BUFFER_OVERFLOW:
        ESP_LOGW(TAG, "MJPEG 帧超过 %u 字节，请增大 MJPEG_FRAME_BYTES",
                 (unsigned)MJPEG_FRAME_BYTES);
        break;
    case UVC_HOST_TRANSFER_ERROR:
        ESP_LOGW(TAG, "USB 传输错误: %d", event->transfer_error.error);
        break;
    default:
        break;
    }
}

static void usb_library_task(void *context)
{
    (void)context;
    while (true) {
        uint32_t flags = 0;
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &flags));
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            usb_host_device_free_all();
        }
    }
}

static void decode_task(void *context)
{
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)context;
    const size_t rgb_bytes = CAPTURE_WIDTH * CAPTURE_HEIGHT * sizeof(uint16_t);
    uint16_t *rgb565 = heap_caps_malloc(rgb_bytes, MALLOC_CAP_SPIRAM);
    assert(rgb565 != NULL);

    const int draw_x = (BSP_LCD_H_RES - CAPTURE_WIDTH) / 2;
    const int draw_y = (BSP_LCD_V_RES - CAPTURE_HEIGHT) / 2;
    uint32_t frame_count = 0;
    int64_t report_started = esp_timer_get_time();

    while (true) {
        uvc_host_frame_t *frame = NULL;
        if (xQueueReceive(frame_queue, &frame, portMAX_DELAY) != pdPASS) {
            continue;
        }

        esp_jpeg_image_cfg_t cfg = {
            .indata = frame->data,
            .indata_size = frame->data_len,
            .outbuf = (uint8_t *)rgb565,
            .outbuf_size = rgb_bytes,
            .out_format = JPEG_IMAGE_FORMAT_RGB565,
            .flags = {.swap_color_bytes = 1},
        };
        esp_jpeg_image_output_t output = {0};
        const esp_err_t err = esp_jpeg_decode(&cfg, &output);
        if (err == ESP_OK && output.width == CAPTURE_WIDTH &&
            output.height == CAPTURE_HEIGHT) {
            ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
                panel, draw_x, draw_y, draw_x + CAPTURE_WIDTH,
                draw_y + CAPTURE_HEIGHT, rgb565));
            frame_count++;
        } else {
            ESP_LOGW(TAG, "JPEG 解码失败: %s, 输出=%ux%u, 输入=%u 字节",
                     esp_err_to_name(err), (unsigned)output.width,
                     (unsigned)output.height, (unsigned)frame->data_len);
        }

        /* A single stream is active, so the current global handle owns frame. */
        uvc_host_frame_return(stream_handle, frame);
        if (frame_count != 0 && frame_count % 60 == 0) {
            const int64_t now = esp_timer_get_time();
            ESP_LOGI(TAG, "显示 %.1f fps",
                     60.0f * 1000000.0f / (float)(now - report_started));
            report_started = now;
        }
    }
}

void app_main(void)
{
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_io_handle_t panel_io = NULL;
    const bsp_display_config_t display_config = {
        .dsi_bus = {
            .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
            .lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS,
        },
    };
    ESP_ERROR_CHECK(bsp_display_new(&display_config, &panel, &panel_io));
    ESP_ERROR_CHECK(bsp_display_brightness_set(80));

    /* The Tab5 BSP controls the USB-A 5 V load switch. */
    ESP_ERROR_CHECK(bsp_feature_enable(BSP_FEATURE_USB, true));
    vTaskDelay(pdMS_TO_TICKS(500));

    frame_queue = xQueueCreate(1, sizeof(uvc_host_frame_t *));
    assert(frame_queue != NULL);

    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    assert(xTaskCreate(usb_library_task, "usb_events", 4096, NULL,
                       USB_TASK_PRIORITY, NULL) == pdPASS);

    const uvc_host_driver_config_t driver_config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = UVC_TASK_PRIORITY,
        .xCoreID = tskNO_AFFINITY,
        .create_background_task = true,
    };
    ESP_ERROR_CHECK(uvc_host_install(&driver_config));
    assert(xTaskCreate(decode_task, "mjpeg_decode", 8192, panel,
                       DECODE_TASK_PRIORITY, NULL) == pdPASS);

    const uvc_host_stream_config_t stream_config = {
        .event_cb = stream_event_callback,
        .frame_cb = frame_callback,
        .usb = {
            .vid = UVC_HOST_ANY_VID,
            .pid = UVC_HOST_ANY_PID,
            .uvc_stream_index = 0,
        },
        .vs_format = {
            .h_res = CAPTURE_WIDTH,
            .v_res = CAPTURE_HEIGHT,
            .fps = CAPTURE_FPS,
            .format = UVC_VS_FORMAT_MJPEG,
        },
        .advanced = {
            .number_of_frame_buffers = UVC_FRAME_BUFFERS,
            .frame_size = MJPEG_FRAME_BYTES,
            .number_of_urbs = UVC_URBS,
            .urb_size = UVC_URB_BYTES,
            .frame_heap_caps = MALLOC_CAP_SPIRAM,
        },
    };

    while (true) {
        ESP_LOGI(TAG, "等待 USB-A UVC 采集卡，目标 MJPEG %ux%u@%u...",
                 CAPTURE_WIDTH, CAPTURE_HEIGHT, CAPTURE_FPS);
        esp_err_t err = uvc_host_stream_open(&stream_config,
                                              pdMS_TO_TICKS(10000),
                                              &stream_handle);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "打开失败: %s；2 秒后重试", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        stream_running = true;
        err = uvc_host_stream_start(stream_handle);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "启动视频流失败: %s", esp_err_to_name(err));
            uvc_host_stream_close(stream_handle);
            stream_handle = NULL;
            stream_running = false;
            continue;
        }
        ESP_LOGI(TAG, "NS 图像流已启动");
        while (stream_running) {
            vTaskDelay(pdMS_TO_TICKS(200));
        }

        /* Let the decoder return its retained frame before closing the stream. */
        vTaskDelay(pdMS_TO_TICKS(100));
        uvc_host_stream_close(stream_handle);
        stream_handle = NULL;
    }
}
