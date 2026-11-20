#include "transport.h"
#include <stdarg.h>
#include <stdatomic.h>
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
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "linkstate.h"
#include "net.h"

typedef struct {
    uint8_t *frame;
    uint16_t len;
    bool reliable;
} tx_item_t;

static QueueHandle_t s_tx_queue;
static frame_handler_t s_handler;
static frame_filter_t s_accept;
static transport_stats_t s_stats;
static vprintf_like_t s_orig_vprintf;
static uint8_t s_parser_store[PROTO_MAX_PAYLOAD + FRAME_OVERHEAD + USB_RX_READ_CHUNK_BYTES];
static SemaphoreHandle_t s_dispatch_mtx;
static linkstate_t s_link;
static _Atomic int s_route = ROUTE_USB, s_active = LINK_NONE;
static TaskHandle_t s_rx_task;
static const char *TAG = "transport";

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

static bool in_dispatch(void)
{
    return s_dispatch_mtx && xSemaphoreGetMutexHolder(s_dispatch_mtx) == xTaskGetCurrentTaskHandle();
}

bool transport_send_to(int route, uint8_t type, const void *payload, size_t len)
{
    return route == ROUTE_USB ? enqueue(type, payload, len, false) : net_send(type, payload, len, false);
}

bool transport_send(uint8_t type, const void *payload, size_t len)
{
    return transport_send_to(atomic_load(&s_route), type, payload, len);
}

bool transport_send_usb(uint8_t type, const void *payload, size_t len)
{
    return enqueue(type, payload, len, false);
}

bool transport_send_reliable(uint8_t type, const void *payload, size_t len)
{
    if (atomic_load(&s_route) == ROUTE_USB) {
        return enqueue(type, payload, len, true);
    }
    /* A handler waiting for room would stall the dispatch the network task needs to drain it. */
    return net_send(type, payload, len, !in_dispatch());
}

static void dispatch(int src, int kind, const proto_frame_t *f)
{
    if (s_accept && !s_accept(f)) {
        return;
    }
    xSemaphoreTake(s_dispatch_mtx, portMAX_DELAY);
    link_verdict_t v = link_on_frame(&s_link, src, kind, f->type, now_ms());
    atomic_store(&s_active, s_link.active);
    atomic_store(&s_route, link_route(&s_link));
    if (v.activated) {
        net_session_activated();
    }
    if (v.handle) {
        s_handler(f, v.live);
    } else if (src == LINK_SRC_NET && (f->type == T_PAIR || f->type == T_FORGET)) {
        ESP_LOGW(TAG, "frame 0x%02x ignored: USB only", f->type);
    }
    xSemaphoreGive(s_dispatch_mtx);
}

void transport_net_frame(int kind, const proto_frame_t *f)
{
    dispatch(LINK_SRC_NET, kind, f);
}

void transport_net_ended(void)
{
    xSemaphoreTake(s_dispatch_mtx, portMAX_DELAY);
    link_net_ended(&s_link);
    atomic_store(&s_active, s_link.active);
    atomic_store(&s_route, link_route(&s_link));
    xSemaphoreGive(s_dispatch_mtx);
}

int transport_route(void)
{
    return atomic_load(&s_route);
}

int transport_link_active(void)
{
    return atomic_load(&s_active);
}

uint32_t transport_rx_stack_hwm(void)
{
    return s_rx_task ? (uint32_t)uxTaskGetStackHighWaterMark(s_rx_task) : 0;
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
                dispatch(LINK_SRC_USB, LINK_USB, &f);
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

void transport_start(frame_handler_t on_frame, frame_filter_t accept)
{
    s_handler = on_frame;
    s_accept = accept;
    link_init(&s_link);
    s_dispatch_mtx = xSemaphoreCreateMutex();
    usb_serial_jtag_driver_config_t cfg = {
        .rx_buffer_size = USB_RX_BUFFER_BYTES,
        .tx_buffer_size = USB_TX_BUFFER_BYTES,
    };
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
    s_tx_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(tx_item_t));
    xTaskCreate(tx_task, "tx", STACK_TX, NULL, PRIO_TX, NULL);
    xTaskCreate(rx_task, "rx", STACK_RX, NULL, PRIO_RX, &s_rx_task);
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);
}
