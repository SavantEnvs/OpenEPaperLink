// mayhem/v2/v2_host.cpp — the ESP32-S3 AP's side of the protocol-v2 radio link
// (ESP32_AP-Flasher/src/v2transport.cpp, compiled unchanged), fed the radio's serial byte stream.
//
// The input is everything the C6/H2 radio sends to the AP over the UART: COBS frames with CRC16
// (oepl-radio-v2.h), each ended by 0x00. The host runs what the AP does after FRM1:
//   v2StartLink()       HELLO, HELLO_REPLY (radio type, caps, block cache), GET_STATS / STATUS
//   ping()              HEARTBEAT / STATUS
//   sendDataAvail()     SET_PENDING (and a PRELOAD request when the radio's cache has room)
//   sendBlock()         a 4 KB block as DATA_CHUNKs with the 4-frame window and retransmissions
//   cacheBlock()        the next block for the radio's block cache
//   setChannelPower()   SET_CONFIG
//   cancelPending()     CANCEL_PENDING
// and then hands the rest of the input to the reader, which handles the radio's events (CHECKIN,
// DATA_NEEDED, BLOCK_SERVED, XFER_COMPLETE/TIMEOUT, TAG_RETURN_DATA, LOG) as rxSerialTask does.
//
// Single-threaded: the reader task (rxSerialTask: onIdle(), then onByte() per byte) runs inside
// vTaskDelay(), one frame per call, and every vTaskDelay() moves the fake clock on by its ticks, so the
// request timeouts expire once the input is used up. What the AP would queue for rxCmdProcessor
// (addRXQueue) is read in full and freed. Per input, v2StartLink() resets the transport's link state
// and the harness resets the globals it owns (apInfo, clock, queue), so each run depends only on its input.
//
// Built as a libFuzzer target (LLVMFuzzerTestOneInput) and, with -DV2_TRACE, as a program that
// prints everything the AP sends, logs and queues, for the known-answer test in mayhem/test.sh.
#include <Arduino.h>
#include <stdio.h>

#include <new>
#include <vector>

#include "../../oepl-radio-v2.h"
#include "commstructs.h"
#include "newproto.h"
#include "radiotransport.h"
#include "serialap.h"
#include "tag_db.h"
#include "web.h"

// ---- globals the AP firmware defines elsewhere (serialap.cpp, tag_db.cpp, newproto.cpp) ----
SemaphoreHandle_t txActive = (SemaphoreHandle_t)1;
volatile uint32_t lastAPActivity = 0;
struct APInfoS apInfo;
struct espSetChannelPower curChannel = {0, 11, 10, 0};
Config config;
std::vector<tagRecord*> tagDB;
RadioTransport* radioTransport = nullptr;
RadioTransport* legacyRadioTransport() { return nullptr; }

// ---- the input and the fake clock ----
static const uint8_t* in;
static size_t inLen, inPos;
static uint32_t nowMs;
static unsigned queued;

uint32_t millis() { return nowMs; }

#ifdef V2_TRACE
static void hex(const char* tag, const uint8_t* p, size_t n) {
    printf("%s", tag);
    for (size_t i = 0; i < n; i++) printf("%02x", p[i]);
    printf("\n");
}
#endif

HostSerial Serial(0);
HostSerial Serial1(1);

size_t HostSerial::write(const uint8_t* buf, size_t n) {
#ifdef V2_TRACE
    if (port_ == 1) {
        // a whole frame per write (writeFrame); show it decoded
        uint8_t raw[V2_MAX_FRAME_RAW];
        const int r = (n > 1 && buf[n - 1] == 0) ? v2CobsDecode(buf, n - 1, raw, sizeof(raw)) : -1;
        if (r >= 5 && v2CheckFrame(raw, r)) {
            ::printf("t=%u TX type=%02x seq=%u flags=%u ", nowMs, raw[0], raw[1], raw[2]);
            hex("payload=", raw + 3, r - 5);
        } else {
            hex("TX BAD ", buf, n);
        }
    } else {
        // the console log, without the firmware's CRs (one line per call)
        ::printf("LOG ");
        for (size_t i = 0; i < n; i++)
            if (buf[i] != '\r' && buf[i] != '\n') putchar(buf[i]);
        ::printf("\n");
    }
#else
    volatile uint8_t sink = 0;
    for (size_t i = 0; i < n; i++) sink ^= buf[i];
    (void)sink;
#endif
    return n;
}

size_t HostSerial::print(const char* s) { return write((const uint8_t*)s, strlen(s)); }

size_t HostSerial::printf(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return 0;
    size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    while (len && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) len--;
    return write((const uint8_t*)buf, len);
}

int HostSerial::available() { return port_ == 1 ? (int)(inLen - inPos) : 0; }

SemaphoreHandle_t xSemaphoreCreateMutex() { return (SemaphoreHandle_t)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t, uint32_t) { return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }

// rxSerialTask's loop body: onIdle(), then the bytes that came in (here: up to the end of one frame)
static void readerStep() {
    if (radioTransport == nullptr) return;
    radioTransport->onIdle();
    while (inPos < inLen) {
        const uint8_t c = in[inPos++];
        radioTransport->onByte(c);
        if (c == 0x00) break;
    }
}

void vTaskDelay(uint32_t ticks) {
    nowMs += ticks ? ticks : 1;
    readerStep();
}

void wsErr(const String& text) { Serial.print(text.c_str()); }

// rxCmdProcessor's queue: the AP reads len bytes of each item (checkCRC, then the process* call)
void addRXQueue(uint8_t* data, uint8_t len, uint8_t type) {
    uint8_t sum = 0;
    for (uint8_t c = 1; c < len; c++) sum += data[c];
    // the structs from the old protocol carry newproto.cpp's checksum; PRELOAD and BLS do not
    const bool hasCrc = type != RX_CMD_PRELOAD && type != RX_CMD_BLS;
    const bool crcOk = len && data[0] == sum;
#ifdef V2_TRACE
    printf("t=%u RXQ type=%u len=%u crc=%s ", nowMs, type, len, hasCrc ? (crcOk ? "ok" : "bad") : "-");
    hex("data=", data, len);
#else
    (void)crcOk;
    (void)hasCrc;
#endif
    queued++;
    free(data);
}

bool sendChannelPower(struct espSetChannelPower* scp) { return radioTransport && radioTransport->setChannelPower(scp); }

// newproto.cpp's addCRC (the S3 checksum of the queued structs), verbatim
void addCRC(void* p, uint8_t len) {
    uint8_t total = 0;
    for (uint8_t c = 1; c < len; c++) total += ((uint8_t*)p)[c];
    ((uint8_t*)p)[0] = total;
}

static void report(const char* what, long v) {
#ifdef V2_TRACE
    printf("t=%u %s -> %ld\n", nowMs, what, v);
#else
    (void)what;
    (void)v;
#endif
}

static uint8_t blockData[2 * V2_BLOCK_SIZE];

static void runSession(const uint8_t* data, size_t size) {
    in = data;
    inLen = size;
    inPos = 0;
    nowMs = 1000;
    queued = 0;
    lastAPActivity = 0;
    apInfo.~APInfoS();
    new (&apInfo) APInfoS();
    config.led = 128;
    config.lock = V2_LOCK_LOCKED;
    radioTransport = nullptr;

    const bool up = v2StartLink();
    report("v2StartLink", up);
    if (!up) return;  // bringAPOnline() falls back to the legacy protocol
#ifdef V2_TRACE
    printf("apInfo type=%02x version=%04x caps=%04x channel=%u power=%u pending=%u nop=%u subghz=%u/%u ",
           apInfo.type, apInfo.version, apInfo.caps, apInfo.channel, apInfo.power, apInfo.pendingBuffer, apInfo.nop,
           apInfo.hasSubGhz, apInfo.SubGhzChannel);
    hex("mac=", apInfo.mac, 8);
#endif
    RadioTransport* t = radioTransport;
    report("ping", t->ping());

    struct pendingData pending = {};
    static const uint8_t mac[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    memcpy(pending.targetMac, mac, 8);
    pending.availdatainfo.dataVer = 0x1122334455667788ull;
    pending.availdatainfo.dataSize = sizeof(blockData) - 100;
    pending.availdatainfo.dataType = 0x21;
    pending.availdatainfo.nextCheckIn = 1;
    pending.attemptsLeft = 10;
    report("sendDataAvail", t->sendDataAvail(&pending));
    report("sendBlock", t->sendBlock(blockData, V2_BLOCK_SIZE, pending.availdatainfo.dataVer, 0, pending.availdatainfo.dataSize));
    report("cacheBlock", t->cacheBlock(blockData + V2_BLOCK_SIZE, V2_BLOCK_SIZE - 100, pending.availdatainfo.dataVer,
                                       V2_BLOCK_SIZE, pending.availdatainfo.dataSize));
    report("setChannelPower", t->setChannelPower(&curChannel));
    report("cancelPending", t->cancelPending(&pending));

    // the rest: events from the radio
    while (inPos < inLen) vTaskDelay(1);
    nowMs += 100;
    readerStep();  // onIdle(): drops an unterminated frame
    report("queued", queued);
}

static void initBlockData() {
    for (size_t i = 0; i < sizeof(blockData); i++) blockData[i] = (uint8_t)(i * 7 + (i >> 8));
}

#ifdef V2_TRACE
int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <radio serial stream>\n", argv[0]);
        return 2;
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    std::vector<uint8_t> buf;
    int c;
    while ((c = fgetc(f)) != EOF) buf.push_back((uint8_t)c);
    fclose(f);
    initBlockData();
    runSession(buf.data(), buf.size());
    return 0;
}
#else
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    static bool init = (initBlockData(), true);
    (void)init;
    runSession(data, size);
    return 0;
}
#endif
