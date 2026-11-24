// G5 known-answer check for oepl_selftest (C: the codec sources are C).
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>
#include "g5/Group5.h"
#include "g5/g5dec.inl"
#include "g5/g5enc.inl"

void checkG5(void);
void checkG5(void) {
    const int w = 64, h = 16;
    // line has room past the 8 bytes of a row: G5DrawLine() writes one byte beyond (w+7)/8 when a black
    // run ends at the right edge of a width that is a multiple of 8
    uint8_t bmp[8 * 16], out[4096], line[16];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < 8; x++) bmp[y * 8 + x] = (uint8_t)((x + y) % 3 ? 0xF0 >> (y % 4) : 0xFF);
    G5ENCIMAGE enc;
    int rc = g5_encode_init(&enc, w, h, out, sizeof out);
    for (int y = 0; y < h && rc == G5_SUCCESS; y++) rc = g5_encode_encodeLine(&enc, bmp + y * 8);
    const int n = g5_encode_getOutSize(&enc);
    G5DECIMAGE dec;
    int same = g5_decode_init(&dec, w, h, out, n) == G5_SUCCESS;
    for (int y = 0; y < h && same; y++) {
        const int r = g5_decode_line(&dec, line);  // the last line returns G5_DECODE_COMPLETE
        same = (r == G5_SUCCESS || r == G5_DECODE_COMPLETE) && !memcmp(line, bmp + y * 8, 8);
    }
    printf("g5 rc=%d size=%d crc=%08lx roundtrip=%s\n", rc, n, crc32(0, out, n), same ? "ok" : "BAD");
}

