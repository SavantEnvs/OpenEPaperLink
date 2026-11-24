// mayhem/v2/mkseeds.cpp — writes the v2-transport seeds: the radio's side of whole sessions, as the
// serial bytes it sends the AP (oepl-radio-v2.h frames). The AP's request seqs follow the order in
// mayhem/v2/v2_host.cpp: HELLO 0, GET_STATS 1, HEARTBEAT 2, SET_PENDING 3, DATA_CHUNK 4..21 (sendBlock),
// 22..38 (cacheBlock), SET_CONFIG 39, CANCEL_PENDING 40.
//   c++ -I. -o mkseeds mayhem/v2/mkseeds.cpp && ./mkseeds mayhem/v2-transport/testsuite
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "oepl-radio-v2.h"

static std::vector<uint8_t> out;
static uint8_t evSeq;

static void frame(uint8_t type, uint8_t seq, uint8_t flags, const void* p, size_t n) {
    uint8_t wire[V2_MAX_FRAME_COBS + 1];
    const size_t w = v2BuildFrame(type, seq, flags, p, n, wire);
    out.insert(out.end(), wire, wire + w);
}
static void resp(uint8_t type, uint8_t seq, const void* p = nullptr, size_t n = 0) { frame(type, seq, V2_FLAG_RESPONSE, p, n); }
static void event(uint8_t type, const void* p, size_t n) { frame(type, evSeq++, 0, p, n); }

static const uint8_t TAG[8] = {0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80};
static const uint64_t VER = 0x1122334455667788ull;

static void hello(uint16_t caps, uint16_t cacheKb, uint8_t major = V2_PROTO_MAJOR) {
    v2HelloReply hr = {};
    hr.protoMajor = major;
    hr.protoMinor = V2_PROTO_MINOR;
    hr.fwVersion = 0x0031;
    hr.hwType = 0xC6;
    for (int i = 0; i < 8; i++) hr.mac[i] = 0xA0 + i;
    hr.caps = caps;
    hr.maxPayload = V2_MAX_PAYLOAD;
    hr.cacheKb = cacheKb;
    hr.pendingSlots = 32;
    resp(V2_HELLO_REPLY, 0, &hr, sizeof(hr));
}
static void status(uint8_t seq, uint16_t crcErrors, uint8_t subghz) {
    v2Status st = {};
    st.uptimeMs = 123456;
    st.channel = 25;
    st.subghzChannel = subghz;
    st.txPowerDbm = 10;
    st.pending = 3;
    st.pendingIdle = 300;
    st.cacheUsedKb = 4;
    st.serialCrcErrors = crcErrors;
    resp(V2_STATUS, seq, &st, sizeof(st));
}
static void events() {
    v2Checkin ci = {};
    memcpy(ci.mac, TAG, 8);
    ci.rssi = -60;
    ci.lqi = 200;
    ci.channel = 25;
    ci.pan = 0x4447;
    for (int i = 0; i < 21; i++) ci.adr[i] = i;
    event(V2_CHECKIN, &ci, sizeof(ci));
    v2DataNeeded dn = {};
    memcpy(dn.mac, TAG, 8);
    dn.dataVer = VER;
    dn.offset = V2_BLOCK_SIZE;
    dn.length = V2_BLOCK_SIZE;
    event(V2_DATA_NEEDED, &dn, sizeof(dn));
    v2BlockServed bs = {};
    memcpy(bs.mac, TAG, 8);
    bs.dataVer = VER;
    bs.blockId = 1;
    bs.partsRequested = 42;
    bs.framesSent = 42;
    bs.lateMs = -3;
    event(V2_BLOCK_SERVED, &bs, sizeof(bs));
    v2XferEnd xe = {};
    memcpy(xe.mac, TAG, 8);
    xe.dataVer = VER;
    event(V2_XFER_COMPLETE, &xe, sizeof(xe));
    event(V2_XFER_TIMEOUT, &xe, sizeof(xe));
    uint8_t trd[sizeof(v2TagReturnData) + 20] = {};
    memcpy(trd, TAG, 8);
    for (size_t i = sizeof(v2TagReturnData); i < sizeof(trd); i++) trd[i] = (uint8_t)i;
    event(V2_TAG_RETURN_DATA, trd, sizeof(trd));
    const char log[] = "\x03radio log line";
    event(V2_LOG, log, sizeof(log) - 1);
    const uint8_t junk[2] = {1, 2};
    event(0x50, junk, sizeof(junk));  // unknown type: NAK
    evSeq--;
    event(V2_XFER_COMPLETE, &xe, sizeof(xe));  // a repeat: only acknowledged
}

static void save(const std::string& dir, const char* name) {
    const std::string path = dir + "/" + name;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f || fwrite(out.data(), 1, out.size(), f) != out.size()) {
        perror(path.c_str());
        exit(1);
    }
    fclose(f);
    out.clear();
    evSeq = 0;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string dir = argv[1];

    // everything confirmed, a 64 KB block cache, then one of each event
    hello(V2_CAP_FRAMING | V2_CAP_BLOCK_CACHE | V2_CAP_SUBGHZ | V2_CAP_TAG_RETURN_DATA | V2_CAP_ALLOW_LIST, 64);
    status(1, 0, V2_NO_SUBGHZ_CHANNEL);
    status(2, 5, 3);
    for (uint8_t s = 3; s <= 40; s++) resp(V2_ACK, s);
    events();
    save(dir, "full_session");

    // no cache; SET_PENDING refused, a chunk refused, SET_CONFIG answered late (retransmitted)
    hello(V2_CAP_FRAMING, 0);
    status(1, 0, V2_NO_SUBGHZ_CHANNEL);
    status(2, 0, V2_NO_SUBGHZ_CHANNEL);
    const uint8_t noSlot = V2_NAK_NO_SLOT, busy = V2_NAK_BUSY;
    resp(V2_NAK, 3, &noSlot, 1);
    resp(V2_ACK, 4);
    resp(V2_NAK, 5, &busy, 1);
    resp(V2_ACK, 6);
    events();
    save(dir, "nak_session");

    // a radio speaking another protocol version
    hello(V2_CAP_FRAMING, 0, 3);
    save(dir, "wrong_version");
    return 0;
}
