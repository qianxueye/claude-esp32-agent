#include "transport.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct {
    uint8_t *frame;
    uint16_t len;
    bool reliable;
} tx_item_t;

static QueueHandle_t s_tx_queue;
static frame_handler_t s_handler;
static transport_stats_t s_stats;
static vprintf_like_t s_orig_vprintf;
static uint8_t s_parser_store[PROTO_MAX_PAYLOAD + FRAME_OVERHEAD + USB_RX_READ_CHUNK_BYTES];

uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool enqueue(uint8_t type, const void *payload, size_t len, bool reliable)
{
    if (!s_tx_queue || len > PROTO_MAX_PAYLOAD) {
        return false;
    }
    if (!usb_serial_jtag_is_connected()) {
        s_stats.tx_dropped++;
        return false;
    }
    size_t total = len + FRAME_OVERHEAD;
    uint8_t *buf = heap_caps_malloc(total, total > 1024 ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL);
    if (!buf) {
        s_stats.tx_dropped++;
        return false;
    }
    proto_encode(buf, type, payload, len);
    tx_item_t it = {.frame = buf, .len = (uint16_t)total, .reliable = reliable};
    TickType_t wait = reliable ? pdMS_TO_TICKS(TX_SNAPSHOT_WRITE_TIMEOUT_MS) : 0;
    if (xQueueSend(s_tx_queue, &it, wait) != pdTRUE) {
        free(buf);
        s_stats.tx_dropped++;
        return false;
    }
    return true;
}

bool transport_send(uint8_t type, const void *payload, size_t len)
{
    return enqueue(type, payload, len, false);
}

bool transport_send_reliable(uint8_t type, const void *payload, size_t len)
{
    return enqueue(type, payload, len, true);
}

void transport_get_stats(transport_stats_t *out)
{
    *out = s_stats;
}

static void tx_task(void *arg)
{
    tx_item_t it;
    for (;;) {
        if (xQueueReceive(s_tx_queue, &it, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        TickType_t wait = it.reliable ? pdMS_TO_TICKS(TX_SNAPSHOT_WRITE_TIMEOUT_MS) : 0;
        int n = usb_serial_jtag_write_bytes(it.frame, it.len, wait);
        if (n == it.len) {
            s_stats.tx_frames++;
            s_stats.tx_bytes += it.len;
        } else {
            s_stats.tx_dropped++;
        }
        free(it.frame);
    }
}

static void rx_task(void *arg)
{
    proto_parser_t parser;
    proto_parser_init(&parser, s_parser_store, sizeof(s_parser_store));
    static uint8_t chunk[USB_RX_READ_CHUNK_BYTES];
    for (;;) {
        int n = usb_serial_jtag_read_bytes(chunk, sizeof(chunk), pdMS_TO_TICKS(USB_RX_POLL_MS));
        size_t off = 0;
        if (n > 0) {
            s_stats.rx_bytes += n;
        }
        do {
            if (n > 0) {
                off += proto_parser_feed(&parser, chunk + off, n - off);
            }
            proto_frame_t f;
            while (proto_parser_next(&parser, now_ms(), &f)) {
                s_handler(&f);
                proto_parser_release(&parser, &f);
            }
        } while (n > 0 && off < (size_t)n);
        s_stats.rx_frames = parser.frames_ok;
        s_stats.rx_crc_errors = parser.crc_errors;
        s_stats.rx_bytes_skipped = parser.bytes_skipped;
    }
}

static int log_vprintf(const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int ret = s_orig_vprintf ? s_orig_vprintf(fmt, ap) : 0;
    if (s_tx_queue && !xPortInIsrContext()) {
        char line[LOG_LINE_MAX_BYTES];
        int n = vsnprintf(line, sizeof(line), fmt, copy);
        if (n > 0) {
            size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                len--;
            }
            if (len) {
                transport_send(T_LOG, line, len);
            }
        }
    }
    va_end(copy);
    return ret;
}

void transport_start(frame_handler_t on_frame)
{
    s_handler = on_frame;
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = USB_RX_BUFFER_BYTES,
        .tx_buffer_size = USB_TX_BUFFER_BYTES,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    s_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(tx_item_t));
    xTaskCreate(tx_task, "tx", STACK_TX, NULL, PRIO_TX, NULL);
    xTaskCreate(rx_task, "rx", STACK_RX, NULL, PRIO_RX, NULL);
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);
}
