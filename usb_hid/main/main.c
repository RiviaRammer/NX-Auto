#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "class/hid/hid_device.h"
#include "config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "tinyusb.h"
#include "tusb.h"

/* The Switch is strict about official Pro Controller USB traffic, so this
 * firmware uses the known-working HORI Pokken Controller USB HID profile. */
#define USB_VID_HORI                 0x0F0D
#define USB_PID_POKKEN               0x0092
#define USB_BCD_DEVICE               0x0100
#define USB_EP_HID_IN                0x81
#define USB_EP_HID_OUT               0x01
#define USB_HID_POLL_MS              5
#define HID_REPORT_MS                15
#define HORI_EP_SIZE                 64

#define DEVICE_NAME                  "Cardputer NS HID"
#define WIFI_MAXIMUM_RETRIES         10
#define WIFI_CONNECTED_BIT           BIT0
#define WIFI_FAILED_BIT              BIT1

#define MACRO_QUEUE_LENGTH           4
#define MACRO_MAX_STEPS              40
#define MACRO_SCRIPT_BODY_MAX        2048
#define TUSB_DESC_TOTAL_LEN          (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

/* M5Stack Cardputer (original) keyboard GPIO matrix. GPIO 8/9/11 select
 * one of eight half-rows; the seven inputs are active-low with pull-ups. */
#define CARDPUTER_KEY_ROWS           4
#define CARDPUTER_KEY_COLUMNS        14
#define CARDPUTER_KEY_DEBOUNCE_SCANS 2

static EventGroupHandle_t wifi_event_group;
static int wifi_retry_count;
static esp_netif_t *wifi_netif;

extern const uint8_t control_page_html_start[] asm("_binary_control_page_html_start");
extern const uint8_t control_page_html_end[] asm("_binary_control_page_html_end");

typedef enum {
    DPAD_UP = 0,
    DPAD_UP_RIGHT = 1,
    DPAD_RIGHT = 2,
    DPAD_DOWN_RIGHT = 3,
    DPAD_DOWN = 4,
    DPAD_DOWN_LEFT = 5,
    DPAD_LEFT = 6,
    DPAD_UP_LEFT = 7,
    DPAD_CENTER = 0x0F,
} dpad_t;

typedef enum {
    BUTTON_Y       = 1u << 0,
    BUTTON_B       = 1u << 1,
    BUTTON_A       = 1u << 2,
    BUTTON_X       = 1u << 3,
    BUTTON_L       = 1u << 4,
    BUTTON_R       = 1u << 5,
    BUTTON_ZL      = 1u << 6,
    BUTTON_ZR      = 1u << 7,
    BUTTON_MINUS   = 1u << 8,
    BUTTON_PLUS    = 1u << 9,
    BUTTON_LSTICK  = 1u << 10,
    BUTTON_RSTICK  = 1u << 11,
    BUTTON_HOME    = 1u << 12,
    BUTTON_CAPTURE = 1u << 13,
} button_t;

typedef struct {
    uint16_t buttons;
    uint8_t dpad;
    uint8_t left_x;
    uint8_t left_y;
    uint8_t right_x;
    uint8_t right_y;
    uint8_t vendor;
} __attribute__((packed)) hori_report_t;

typedef struct {
    uint16_t duration_ms;
    uint16_t buttons;
    dpad_t dpad;
    uint8_t left_x;
    uint8_t left_y;
    uint8_t right_x;
    uint8_t right_y;
} macro_step_t;

typedef enum {
    MACRO_COMMAND_STOP,
    MACRO_COMMAND_TAP,
    MACRO_COMMAND_SCRIPT,
} macro_command_type_t;

typedef struct {
    macro_command_type_t type;
    uint16_t buttons;
    uint16_t duration_ms;
    dpad_t dpad;
    uint8_t step_count;
    macro_step_t steps[MACRO_MAX_STEPS];
} macro_command_t;

typedef struct {
    bool active;
    uint8_t step;
    uint8_t step_count;
    uint32_t run_id;
} macro_status_t;

static QueueHandle_t macro_queue;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static macro_status_t macro_status;

static const gpio_num_t keyboard_select_pins[] = {
    GPIO_NUM_8, GPIO_NUM_9, GPIO_NUM_11,
};
static const gpio_num_t keyboard_input_pins[] = {
    GPIO_NUM_13, GPIO_NUM_15, GPIO_NUM_3, GPIO_NUM_4,
    GPIO_NUM_5, GPIO_NUM_6, GPIO_NUM_7,
};
static uint64_t keyboard_candidate;
static uint64_t keyboard_stable;
static uint8_t keyboard_candidate_scans;

static hori_report_t gamepad_report = {
    .dpad = DPAD_CENTER,
    .left_x = 0x80,
    .left_y = 0x80,
    .right_x = 0x80,
    .right_y = 0x80,
};

static const tusb_desc_device_t device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID_HORI,
    .idProduct = USB_PID_POKKEN,
    .bcdDevice = USB_BCD_DEVICE,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x00,
    .bNumConfigurations = 0x01,
};

static const char *string_descriptor[] = {
    (const char[]){0x09, 0x04},
    "HORI CO.,LTD.",
    "POKKEN CONTROLLER",
};

static const uint8_t hid_report_descriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
    0x15, 0x00, 0x25, 0x01, 0x35, 0x00, 0x45, 0x01,
    0x75, 0x01, 0x95, 0x10,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x10, 0x81, 0x02,
    0x05, 0x01, 0x25, 0x07, 0x46, 0x3B, 0x01,
    0x75, 0x04, 0x95, 0x01, 0x65, 0x14, 0x09, 0x39, 0x81, 0x42,
    0x65, 0x00, 0x95, 0x01, 0x81, 0x01,
    0x26, 0xFF, 0x00, 0x46, 0xFF, 0x00,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x32, 0x09, 0x35,
    0x75, 0x08, 0x95, 0x04, 0x81, 0x02,
    0x06, 0x00, 0xFF, 0x09, 0x20, 0x95, 0x01, 0x81, 0x02,
    0x0A, 0x21, 0x26, 0x95, 0x08, 0x91, 0x02,
    0xC0,
};

static const uint8_t hid_configuration_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, 0, 500),
    TUD_HID_INOUT_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE,
                             sizeof(hid_report_descriptor),
                             USB_EP_HID_OUT, USB_EP_HID_IN,
                             HORI_EP_SIZE, USB_HID_POLL_MS),
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type, uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    uint16_t len = sizeof(gamepad_report);
    if (len > reqlen) {
        len = reqlen;
    }
    memcpy(buffer, &gamepad_report, len);
    return len;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

static macro_step_t step(uint16_t duration_ms, uint16_t buttons, dpad_t dpad)
{
    return (macro_step_t){
        .duration_ms = duration_ms,
        .buttons = buttons,
        .dpad = dpad,
        .left_x = 0x80,
        .left_y = 0x80,
        .right_x = 0x80,
        .right_y = 0x80,
    };
}

static void apply_step(const macro_step_t *current)
{
    gamepad_report.buttons = current->buttons;
    gamepad_report.dpad = current->dpad;
    gamepad_report.left_x = current->left_x;
    gamepad_report.left_y = current->left_y;
    gamepad_report.right_x = current->right_x;
    gamepad_report.right_y = current->right_y;
    gamepad_report.vendor = 0;
}

static void set_neutral_report(void)
{
    const macro_step_t neutral = step(0, 0, DPAD_CENTER);
    apply_step(&neutral);
}

static void init_cardputer_keyboard(void)
{
    gpio_config_t select_config = {
        .pin_bit_mask = BIT64(GPIO_NUM_8) | BIT64(GPIO_NUM_9) |
                        BIT64(GPIO_NUM_11),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&select_config));
    for (size_t i = 0; i < sizeof(keyboard_select_pins) /
                                sizeof(keyboard_select_pins[0]); ++i) {
        ESP_ERROR_CHECK(gpio_set_level(keyboard_select_pins[i], 0));
    }

    gpio_config_t input_config = {
        .pin_bit_mask = BIT64(GPIO_NUM_13) | BIT64(GPIO_NUM_15) |
                        BIT64(GPIO_NUM_3) | BIT64(GPIO_NUM_4) |
                        BIT64(GPIO_NUM_5) | BIT64(GPIO_NUM_6) |
                        BIT64(GPIO_NUM_7),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_config));
}

static uint64_t scan_cardputer_keyboard(void)
{
    uint64_t keys = 0;

    for (uint8_t selector = 0; selector < 8; ++selector) {
        for (uint8_t bit = 0; bit < 3; ++bit) {
            gpio_set_level(keyboard_select_pins[bit],
                           (selector >> bit) & 1u);
        }

        for (uint8_t input = 0;
             input < sizeof(keyboard_input_pins) /
                         sizeof(keyboard_input_pins[0]);
             ++input) {
            if (gpio_get_level(keyboard_input_pins[input]) != 0) {
                continue;
            }

            const uint8_t row = 3u - (selector & 3u);
            const uint8_t column = (uint8_t)(input * 2u) +
                                   (selector < 4u ? 1u : 0u);
            keys |= UINT64_C(1) << (row * CARDPUTER_KEY_COLUMNS + column);
        }
    }
    return keys;
}

static bool cardputer_key_pressed(uint64_t keys, uint8_t row, uint8_t column)
{
    return (keys & (UINT64_C(1) <<
                    (row * CARDPUTER_KEY_COLUMNS + column))) != 0;
}

static dpad_t cardputer_dpad(bool up, bool down, bool left, bool right)
{
    if (up == down) {
        up = false;
        down = false;
    }
    if (left == right) {
        left = false;
        right = false;
    }

    if (up && right) return DPAD_UP_RIGHT;
    if (down && right) return DPAD_DOWN_RIGHT;
    if (down && left) return DPAD_DOWN_LEFT;
    if (up && left) return DPAD_UP_LEFT;
    if (up) return DPAD_UP;
    if (right) return DPAD_RIGHT;
    if (down) return DPAD_DOWN;
    if (left) return DPAD_LEFT;
    return DPAD_CENTER;
}

static void update_cardputer_keyboard(void)
{
    const uint64_t scanned = scan_cardputer_keyboard();
    if (scanned != keyboard_candidate) {
        keyboard_candidate = scanned;
        keyboard_candidate_scans = 1;
    } else if (keyboard_candidate_scans < CARDPUTER_KEY_DEBOUNCE_SCANS) {
        keyboard_candidate_scans++;
        if (keyboard_candidate_scans == CARDPUTER_KEY_DEBOUNCE_SCANS) {
            keyboard_stable = keyboard_candidate;
        }
    }
}

static void apply_cardputer_keyboard(void)
{
    const uint64_t keys = keyboard_stable;
    const bool fn = cardputer_key_pressed(keys, 2, 0);
    const bool shift = cardputer_key_pressed(keys, 2, 1);
    uint16_t buttons = 0;

    if (cardputer_key_pressed(keys, 2, 2)) buttons |= BUTTON_A;
    if (cardputer_key_pressed(keys, 3, 7)) buttons |= BUTTON_B;
    if (cardputer_key_pressed(keys, 3, 4)) buttons |= BUTTON_X;
    if (cardputer_key_pressed(keys, 1, 6)) buttons |= BUTTON_Y;
    if (cardputer_key_pressed(keys, 2, 10)) buttons |= BUTTON_L;
    if (cardputer_key_pressed(keys, 1, 4)) buttons |= BUTTON_R;
    if (shift && cardputer_key_pressed(keys, 0, 12)) buttons |= BUTTON_PLUS;
    if (shift && cardputer_key_pressed(keys, 0, 11)) buttons |= BUTTON_MINUS;

    const bool up = fn && cardputer_key_pressed(keys, 2, 11);
    const bool left = fn && cardputer_key_pressed(keys, 3, 10);
    const bool down = fn && cardputer_key_pressed(keys, 3, 11);
    const bool right = fn && cardputer_key_pressed(keys, 3, 12);
    const macro_step_t physical = step(
        0, buttons, cardputer_dpad(up, down, left, right));
    apply_step(&physical);
}

static void publish_status(bool active, uint8_t current_step, uint8_t step_count)
{
    portENTER_CRITICAL(&status_lock);
    macro_status.active = active;
    macro_status.step = current_step;
    macro_status.step_count = step_count;
    portEXIT_CRITICAL(&status_lock);
}

static macro_status_t read_status(void)
{
    macro_status_t result;
    portENTER_CRITICAL(&status_lock);
    result = macro_status;
    portEXIT_CRITICAL(&status_lock);
    return result;
}

static void hid_report_task(void *arg)
{
    (void)arg;
    macro_step_t steps[MACRO_MAX_STEPS];
    size_t step_count = 0;
    size_t current_step = 0;
    TickType_t step_started = 0;
    bool active = false;
    macro_command_t command;

    set_neutral_report();
    while (true) {
        update_cardputer_keyboard();

        if (xQueueReceive(macro_queue, &command, 0) == pdTRUE) {
            if (command.type == MACRO_COMMAND_STOP) {
                active = false;
                set_neutral_report();
                publish_status(false, 0, 0);
            } else if (active) {
            } else if (command.type == MACRO_COMMAND_SCRIPT) {
                step_count = command.step_count;
                memcpy(steps, command.steps,
                       step_count * sizeof(macro_step_t));
                current_step = 0;
                step_started = xTaskGetTickCount();
                active = true;
                apply_step(&steps[0]);
                portENTER_CRITICAL(&status_lock);
                macro_status.run_id++;
                portEXIT_CRITICAL(&status_lock);
                publish_status(true, 1, step_count);
            } else {
                steps[0] = step(command.duration_ms, command.buttons, command.dpad);
                step_count = 1;
                current_step = 0;
                step_started = xTaskGetTickCount();
                active = true;
                apply_step(&steps[0]);
                portENTER_CRITICAL(&status_lock);
                macro_status.run_id++;
                portEXIT_CRITICAL(&status_lock);
                publish_status(true, 1, step_count);
            }
        }

        if (active) {
            const TickType_t elapsed = xTaskGetTickCount() - step_started;
            if (elapsed >= pdMS_TO_TICKS(steps[current_step].duration_ms)) {
                current_step++;
                step_started = xTaskGetTickCount();
                if (current_step >= step_count) {
                    active = false;
                    set_neutral_report();
                    publish_status(false, step_count, step_count);
                } else {
                    apply_step(&steps[current_step]);
                    publish_status(true, current_step + 1, step_count);
                }
            }
        }

        if (!active) {
            apply_cardputer_keyboard();
        }

        if (tud_mounted() && tud_hid_ready()) {
            tud_hid_report(0, &gamepad_report, sizeof(gamepad_report));
        }
        vTaskDelay(pdMS_TO_TICKS(HID_REPORT_MS));
    }
}

static bool parse_u16_query(const char *query, const char *key,
                            uint16_t minimum, uint16_t maximum,
                            uint16_t *value)
{
    char text[12];
    if (httpd_query_key_value(query, key, text, sizeof(text)) != ESP_OK) {
        return false;
    }
    char *end = NULL;
    long parsed = strtol(text, &end, 10);
    if (end == text || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    *value = (uint16_t)parsed;
    return true;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const size_t length = control_page_html_end - control_page_html_start;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)control_page_html_start, length);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    const macro_status_t status = read_status();
    esp_netif_ip_info_t ip_info = {0};
    if (wifi_netif != NULL) {
        esp_netif_get_ip_info(wifi_netif, &ip_info);
    }
    char json[256];
    snprintf(json, sizeof(json),
             "{\"usb_connected\":%s,\"usb_mounted\":%s,"
             "\"usb_suspended\":%s,\"hid_ready\":%s,\"active\":%s,"
             "\"step\":%u,\"step_count\":%u,\"run_id\":%lu,"
             "\"ip\":\"" IPSTR "\"}",
             tud_connected() ? "true" : "false",
             tud_mounted() ? "true" : "false",
             tud_suspended() ? "true" : "false",
             tud_hid_ready() ? "true" : "false",
             status.active ? "true" : "false", status.step,
             status.step_count, (unsigned long)status.run_id,
             IP2STR(&ip_info.ip));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t send_http_error(httpd_req_t *req, const char *status,
                                 const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, message);
}

static esp_err_t stop_post_handler(httpd_req_t *req)
{
    const macro_command_t command = {.type = MACRO_COMMAND_STOP};
    xQueueReset(macro_queue);
    if (xQueueSend(macro_queue, &command, 0) != pdTRUE) {
        return send_http_error(req, "503 Service Unavailable", "macro queue is full");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"stopped\":true}");
}

static esp_err_t script_post_handler(httpd_req_t *req)
{
    if (!tud_mounted()) {
        return send_http_error(req, "409 Conflict",
                               "Nintendo Switch USB is not mounted");
    }
    if (read_status().active) {
        return send_http_error(req, "409 Conflict", "another input is running");
    }
    if (req->content_len == 0 || req->content_len > MACRO_SCRIPT_BODY_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "script body is empty or too large");
    }

    char *body = malloc(req->content_len + 1);
    if (body == NULL) {
        return send_http_error(req, "503 Service Unavailable",
                               "not enough memory for script");
    }
    size_t received = 0;
    while (received < req->content_len) {
        const int result = httpd_req_recv(req, body + received,
                                          req->content_len - received);
        if (result <= 0) {
            free(body);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "failed to receive script");
        }
        received += (size_t)result;
    }
    body[received] = '\0';

    macro_command_t command = {.type = MACRO_COMMAND_SCRIPT};
    char *save = NULL;
    for (char *line = strtok_r(body, "\n", &save);
         line != NULL;
         line = strtok_r(NULL, "\n", &save)) {
        unsigned duration;
        unsigned buttons;
        unsigned dpad;
        unsigned left_x;
        unsigned left_y;
        unsigned right_x;
        unsigned right_y;
        char trailing;
        if (command.step_count >= MACRO_MAX_STEPS ||
            sscanf(line, "%u,%u,%u,%u,%u,%u,%u%c",
                   &duration, &buttons, &dpad, &left_x, &left_y,
                   &right_x, &right_y, &trailing) != 7 ||
            duration < 15 || duration > 5000 || buttons > 0x3FFF ||
            (dpad > DPAD_UP_LEFT && dpad != DPAD_CENTER) ||
            left_x > 255 || left_y > 255 ||
            right_x > 255 || right_y > 255) {
            free(body);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                       "invalid compiled script step");
        }

        macro_step_t *current = &command.steps[command.step_count++];
        *current = (macro_step_t){
            .duration_ms = (uint16_t)duration,
            .buttons = (uint16_t)buttons,
            .dpad = (dpad_t)dpad,
            .left_x = (uint8_t)left_x,
            .left_y = (uint8_t)left_y,
            .right_x = (uint8_t)right_x,
            .right_y = (uint8_t)right_y,
        };
    }
    free(body);

    if (command.step_count == 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "script has no steps");
    }
    if (xQueueSend(macro_queue, &command, 0) != pdTRUE) {
        return send_http_error(req, "503 Service Unavailable",
                               "input queue is full");
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true}");
}

static esp_err_t tap_post_handler(httpd_req_t *req)
{
    if (!tud_mounted()) {
        return send_http_error(req, "409 Conflict",
                               "Nintendo Switch USB is not mounted");
    }
    if (read_status().active) {
        return send_http_error(req, "409 Conflict", "another input is running");
    }

    char query[128];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "missing input parameters");
    }

    uint16_t dpad_value;
    macro_command_t command = {.type = MACRO_COMMAND_TAP};
    bool valid =
        parse_u16_query(query, "buttons", 0, 0x3FFF, &command.buttons) &&
        parse_u16_query(query, "dpad", 0, DPAD_CENTER, &dpad_value) &&
        parse_u16_query(query, "duration", 15, 1000, &command.duration_ms);
    if (!valid || (dpad_value > DPAD_UP_LEFT && dpad_value != DPAD_CENTER)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "invalid input parameter");
    }
    command.dpad = (dpad_t)dpad_value;

    if (xQueueSend(macro_queue, &command, 0) != pdTRUE) {
        return send_http_error(req, "503 Service Unavailable", "input queue is full");
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true}");
}

static void start_web_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 5;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler},
        {.uri = "/api/stop", .method = HTTP_POST, .handler = stop_post_handler},
        {.uri = "/api/tap", .method = HTTP_POST, .handler = tap_post_handler},
        {.uri = "/api/script", .method = HTTP_POST, .handler = script_post_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_retry_count < WIFI_MAXIMUM_RETRIES) {
            wifi_retry_count++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_FAILED_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_retry_count = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void start_wifi_station(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_set_hostname(wifi_netif, DEVICE_NAME));
    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        abort();
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));

    esp_event_handler_instance_t wifi_handler;
    esp_event_handler_instance_t ip_handler;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &wifi_handler));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &ip_handler));

    wifi_config_t config = {
        .sta = {
            .ssid = CARDPUTER_WIFI_SSID,
            .password = CARDPUTER_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {.capable = true, .required = false},
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());

    const EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);
    (void)bits;
}

static void init_nvs(void)
{
    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(result);
}

void app_main(void)
{
    init_nvs();
    init_cardputer_keyboard();

    macro_queue = xQueueCreate(MACRO_QUEUE_LENGTH, sizeof(macro_command_t));
    if (macro_queue == NULL) {
        abort();
    }

    const tinyusb_config_t tusb_cfg = {
        .device_descriptor = &device_descriptor,
        .string_descriptor = string_descriptor,
        .string_descriptor_count = sizeof(string_descriptor) / sizeof(string_descriptor[0]),
        .external_phy = false,
#if (TUD_OPT_HIGH_SPEED)
        .fs_configuration_descriptor = hid_configuration_descriptor,
        .hs_configuration_descriptor = hid_configuration_descriptor,
        .qualifier_descriptor = NULL,
#else
        .configuration_descriptor = hid_configuration_descriptor,
#endif
    };

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    xTaskCreate(hid_report_task, "hid_report", 4096, NULL, 5, NULL);
    start_wifi_station();
    start_web_server();
}
