// Writes seed inputs for c6_radio_fw / c6_uart_link (see c6_host.c for the record format).
// Not part of the build: run by hand, the outputs are committed under mayhem/<target>/testsuite.
#include <stdio.h>
#include <string.h>
#include "proto.h"
#include "v2link.h"

static uint8_t buf[8192];
static size_t n;
static int uartOnly;
static uint8_t seq;
static const uint8_t tagMac[8] = {1, 2, 3, 4, 5, 6, 7, 8};

static void ser(const void *p, size_t len) {
    if (uartOnly) { memcpy(buf + n, p, len); n += len; return; }
    const uint8_t *b = p;
    while (len) {  // a serial record carries at most 255 bytes
        size_t k = len > 255 ? 255 : len;
        buf[n++] = 1; buf[n++] = (uint8_t)k;
        memcpy(buf + n, b, k); n += k; b += k; len -= k;
    }
}
static void frame(uint8_t type, const void *pl, size_t len) {
    uint8_t wire[V2_MAX_FRAME_COBS + 1];
    size_t w = v2BuildFrame(type, seq++, 0, pl, len, wire);
    ser(wire, w);
}
static void rf(const uint8_t *p, size_t len) {  // adds the 2-byte FCS the PHY length includes
    buf[n++] = 0; buf[n++] = (uint8_t)(len + 2);
    memcpy(buf + n, p, len); n += len; buf[n++] = 0; buf[n++] = 0;
}
static void sum(uint8_t *p, size_t len) { uint8_t t = 0; for (size_t i = 1; i < len; i++) t += p[i]; p[0] = t; }
static void bcast(uint8_t *f, size_t *len, uint8_t type) {
    struct MacFrameBcast *h = (void *)f; memset(h, 0, sizeof *h);
    h->fcs.frameType = 1; h->fcs.destAddrType = 2; h->fcs.srcAddrType = 3; h->dstPan = PROTO_PAN_ID; h->dstAddr = 0xFFFF; h->srcPan = PROTO_PAN_ID;
    memcpy(h->src, tagMac, 8); f[sizeof *h] = type; *len = sizeof *h + 1;
}
static void normal(uint8_t *f, size_t *len, uint8_t type) {
    struct MacFrameNormal *h = (void *)f; memset(h, 0, sizeof *h);
    h->fcs.frameType = 1; h->fcs.destAddrType = 3; h->fcs.srcAddrType = 3; h->fcs.panIdCompressed = 1; h->pan = PROTO_PAN_ID;
    memcpy(h->src, tagMac, 8); f[sizeof *h] = type; *len = sizeof *h + 1;
}
static void out(const char *path) { FILE *f = fopen(path, "wb"); fwrite(buf, 1, n, f); fclose(f); n = 0; seq = 0; }

static void s3Setup(void) {
    ser("NFO?", 4); ser("FRM1", 4);
    struct v2Hello h = {V2_PROTO_MAJOR, V2_PROTO_MINOR, 0xFFFF}; frame(V2_HELLO, &h, sizeof h);
    struct v2SetConfig sc = {25, 0xFF, 10, 0, 255}; frame(V2_SET_CONFIG, &sc, sizeof sc);
    struct v2SetPending sp; memset(&sp, 0, sizeof sp);
    memcpy(sp.mac, tagMac, 8); sp.dataVer = 0x1122334455667788ULL; sp.dataSize = 600; sp.dataType = 0x20; sp.nextCheckin = 1; sp.ttlMin = 10; sp.flags = V2_PENDING_DATA_FOLLOWS;
    frame(V2_SET_PENDING, &sp, sizeof sp);
    for (uint32_t off = 0; off < 600; off += V2_CHUNK_MAX) {
        const uint32_t k = 600 - off < V2_CHUNK_MAX ? 600 - off : V2_CHUNK_MAX;
        uint8_t c[sizeof(struct v2DataChunk) + V2_CHUNK_MAX]; struct v2DataChunk *dc = (void *)c;
        dc->dataVer = sp.dataVer; dc->offset = off; dc->totalSize = 600;
        for (uint32_t i = 0; i < k; i++) dc->bytes[i] = (uint8_t)(off + i);
        frame(V2_DATA_CHUNK, c, sizeof(struct v2DataChunk) + k);
    }
    frame(V2_GET_STATS, NULL, 0);
}

int main(int argc, char **argv) {
    const char *dir = argv[1];
    char p[512];
    uint8_t f[128]; size_t l;
    // radio: full check-in, block request, xfer complete, ping, tag return data
    s3Setup();
    bcast(f, &l, PKT_AVAIL_DATA_REQ); struct AvailDataReq *adr = (void *)(f + l); memset(adr, 0, sizeof *adr);
    adr->hwType = 0x33; adr->batteryMv = 2900; adr->currentChannel = 25; sum((uint8_t *)adr, sizeof *adr); l += sizeof *adr; rf(f, l);
    normal(f, &l, PKT_BLOCK_REQUEST); struct blockRequest *br = (void *)(f + l); memset(br, 0, sizeof *br);
    br->ver = 0x1122334455667788ULL; br->blockId = 0; br->type = 0x20; memset(br->requestedParts, 0xFF, BLOCK_REQ_PARTS_BYTES);
    sum((uint8_t *)br, sizeof *br); l += sizeof *br; rf(f, l);
    buf[n++] = 2; buf[n++] = 20;
    normal(f, &l, PKT_XFER_COMPLETE); rf(f, l);
    bcast(f, &l, PKT_PING); f[l++] = 25; rf(f, l);
    normal(f, &l, PKT_TAG_RETURN_DATA); memset(f + l, 0x5A, 30); l += 30; rf(f, l);
    buf[n++] = 3;
    frame(V2_HEARTBEAT, NULL, 0);
    snprintf(p, sizeof p, "%s/c6-radio-fw/testsuite/checkin_block_xfer", dir); out(p);
    // radio: short check-in, nothing pending
    ser("FRM1", 4);
    bcast(f, &l, PKT_AVAIL_DATA_SHORTREQ); memset(f + l, 0, 3); l += 3; rf(f, l);
    snprintf(p, sizeof p, "%s/c6-radio-fw/testsuite/short_checkin", dir); out(p);
    // uart only: the S3 setup sequence plus allow-list and drop
    uartOnly = 1;
    s3Setup();
    uint8_t macs[16]; memcpy(macs, tagMac, 8); memset(macs + 8, 0xAB, 8);
    struct v2SetConfig sc = {0xFF, 0xFF, 0x7F, V2_LOCK_LOCKED << V2_CFG_LOCK_SHIFT, 10};
    frame(V2_ALLOW_ADD, macs, 16); frame(V2_SET_CONFIG, &sc, sizeof sc); frame(V2_ALLOW_REMOVE, macs + 8, 8);
    struct v2CancelPending cp; memcpy(cp.mac, tagMac, 8); frame(V2_CANCEL_PENDING, &cp, sizeof cp);
    uint64_t ver = 0x1122334455667788ULL; frame(V2_DATA_DROP, &ver, 8); frame(V2_ALLOW_CLEAR, NULL, 0); frame(V2_HEARTBEAT, NULL, 0);
    snprintf(p, sizeof p, "%s/c6-uart-link/testsuite/s3_session", dir); out(p);
    ser("RDY?NFO?", 8);
    snprintf(p, sizeof p, "%s/c6-uart-link/testsuite/ascii", dir); out(p);
    return 0;
}
