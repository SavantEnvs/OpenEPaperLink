// Host board layer for the OpenEPaperLink C6/H2 radio firmware (ARM_Tag_FW/OpenEPaperLink_esp32_C6_AP/main).
//
// Replaces the hardware files (radio.c, second_uart.c, led.c, utils.c, SubGigRadio.c) and runs the
// firmware's own app_main() unchanged. The input file is what the outside world sends the radio:
//
//   C6_UART_ONLY (target c6_uart_link): the whole file is the serial stream from the ESP32-S3,
//     delivered in chunks of up to 256 bytes per main-loop pass (ASCII commands, "FRM1", then
//     COBS/CRC protocol-v2 frames).
//   default (target c6_radio_fw): a sequence of records, each starting with a tag byte:
//     tag%4 == 0  802.15.4 frame from a tag: <len> then len%128 PHY bytes (incl. 2-byte FCS)
//     tag%4 == 1  serial bytes from the S3:   <len> then len bytes
//     tag%4 == 2  time passes:                <n>   then the clock advances n*10 ms
//     tag%4 == 3  time passes:                one housekeeping interval (60 s)
//
// The firmware's main loop never returns; once the input is used up the program exits, the way a
// stream reader stops at end of file. With C6_TRACE every byte the firmware sends (serial and radio)
// is printed as hex, which mayhem/test.sh compares against golden output.
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void app_main(void);

static uint8_t *in;
static size_t inLen, inPos;
static uint32_t nowMs = 1000;
static const uint8_t *serPending;
static size_t serPendingLen;

uint8_t mSelfMac[8] = {0x00, 0x12, 0x4B, 0x00, 0x1A, 0x2B, 0x3C, 0x4D};
TaskHandle_t radioWakeTask;
int64_t lastRxAgeUs = 0;
int64_t lastRxSfdUs = 0;
int8_t lastRxRssi = 0;
uint8_t lastRxLqi = 0;
uint32_t radioRxFrames = 0;
uint32_t radioTxFrames = 0;
volatile uint32_t uartOverflows = 0;

static void trace(const char *what, const uint8_t *p, size_t n) {
#ifdef C6_TRACE
    printf("%s", what);
    for (size_t i = 0; i < n; i++) printf("%02X", p[i]);
    printf("\n");
#else
    (void)what; (void)p; (void)n;
#endif
}

static void endOfInput(void) {
#ifdef C6_TRACE
    fflush(stdout);
#endif
    free(in);
    exit(0);
}

// --- utils.c / led.c ---
uint32_t getMillis(void) { return nowMs; }
void delay(int ms) { nowMs += (uint32_t)ms; }
void init_nvs(void) {}
void init_led(void) {}
void led_flash(int nr) { (void)nr; }
void led_set_brightness(uint8_t b) { (void)b; }

// --- FreeRTOS / ESP-IDF ---
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)&radioWakeTask; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks) { (void)clear; nowMs += ticks; return 0; }
esp_err_t esp_event_loop_create_default(void) { return ESP_OK; }
int esp_reset_reason(void) { return 1; }
esp_err_t uart_wait_tx_done(int port, TickType_t ticks) { (void)port; (void)ticks; return ESP_OK; }
size_t heap_caps_get_free_size(uint32_t caps) { (void)caps; return 64 * 1024; }
int64_t esp_timer_get_time(void) { return (int64_t)nowMs * 1000; }

// --- radio.c ---
void radio_init(uint8_t ch) { (void)ch; }
void radioSetChannel(uint8_t ch) { (void)ch; }
void radioSetTxPower(uint8_t p) { (void)p; }
bool radioTxBusy(void) { return false; }
bool radioTx(uint8_t *packet) {
    radioTxFrames++;
    trace("RF> ", packet, (size_t)packet[0] + 1 > 130 ? 130 : (size_t)packet[0] + 1);
    return true;
}

#ifdef C6_UART_ONLY
int8_t commsRxUnencrypted(uint8_t *data) {
    (void)data;
    if (inPos >= inLen) endOfInput();
    size_t n = inLen - inPos;
    if (n > 256) n = 256;
    serPending = in + inPos;
    serPendingLen = n;
    inPos += n;
    return 0;
}
#else
int8_t commsRxUnencrypted(uint8_t *data) {
    for (;;) {
        if (inPos + 1 >= inLen) endOfInput();
        const uint8_t tag = in[inPos++];
        uint8_t arg = in[inPos++];
        switch (tag & 3) {
            case 0: {  // as esp_ieee802154_receive_done() queues it and commsRxUnencrypted() returns it
                static uint8_t frame[130];
                size_t n = arg % 128;
                if (n > inLen - inPos) n = inLen - inPos;
                memset(frame, 0, sizeof(frame));
                frame[0] = (uint8_t)n;
                memcpy(frame + 1, in + inPos, n);
                inPos += n;
                lastRxAgeUs = 0;
                lastRxSfdUs = esp_timer_get_time();
                radioRxFrames++;
                memcpy(data, &frame[1], frame[0] + 1);
                return (int8_t)(frame[0] - 2);
            }
            case 1: {
                size_t n = arg;
                if (n > inLen - inPos) n = inLen - inPos;
                serPending = in + inPos;
                serPendingLen = n;
                inPos += n;
                return 0;
            }
            case 2:
                nowMs += (uint32_t)arg * 10;
                return 0;
            default:
                inPos--;  // no argument
                nowMs += 60000;
                return 0;
        }
    }
}
#endif

// --- second_uart.c ---
void init_second_uart(void) {}
void uart_switch_speed(int baud) { (void)baud; }
void uartFlushInput(void) { serPendingLen = 0; }
int uartRead(uint8_t *buf, int max) {
    if (!serPendingLen) return 0;
    size_t n = serPendingLen < (size_t)max ? serPendingLen : (size_t)max;
    memcpy(buf, serPending, n);
    serPending += n;
    serPendingLen -= n;
    return (int)n;
}
void uartWrite(const uint8_t *data, size_t len) { trace("SER> ", data, len); }
void uartTx(uint8_t data) { uartWrite(&data, 1); }
void uart_printf(const char *format, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
    trace("TXT> ", (const uint8_t *)buf, (size_t)n);
}

int main(int argc, char **argv) {
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : stdin;
    if (!f) { perror("open"); return 1; }
    size_t cap = 4096;
    in = malloc(cap);
    size_t r;
    while ((r = fread(in + inLen, 1, cap - inLen, f)) > 0) {
        inLen += r;
        if (inLen == cap) in = realloc(in, cap *= 2);
    }
    if (f != stdin) fclose(f);
    app_main();
    return 0;
}
