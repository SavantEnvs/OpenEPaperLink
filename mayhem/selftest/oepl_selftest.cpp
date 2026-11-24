// Known-answer checks for the host-buildable OpenEPaperLink code, run by mayhem/test.sh.
// Each check prints one result line; test.sh compares the lines with mayhem/selftest/expected.txt.
//   deflate: miniz-oepl tdefl_compressOEPL of a fixed image-like buffer (as makeimage.cpp), inflated by zlib
//   g5:      G5 encode of a fixed 1-bpp pattern, decoded again
//   ttf:     TinyTTF render of a fixed string with the AP's Signika-SB.ttf into a 296x128 1-bpp sprite
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "miniz-oepl.h"
#include "TinyTTF.h"
extern "C" void checkG5(void);  // g5_selftest.c

static void checkDeflate(void) {
    static uint8_t img[296 * 128 / 8];
    for (size_t i = 0; i < sizeof img; i++) img[i] = (i % 37 < 20) ? 0xFF : (uint8_t)(i * 7);
    Miniz::tdefl_compressor *c = (Miniz::tdefl_compressor *)calloc(1, sizeof *c);
    static uint8_t z[8192];
    size_t inB = sizeof img, outB = sizeof z;
    Miniz::tdefl_initOEPL(c, NULL, NULL, Miniz::TDEFL_WRITE_ZLIB_HEADER | 1500, 4096);
    const int st = Miniz::tdefl_compressOEPL(c, img, &inB, z, &outB, Miniz::TDEFL_FINISH);
    static uint8_t back[sizeof img];
    uLongf bl = sizeof back;
    const int rc = uncompress(back, &bl, z, outB);
    printf("deflate status=%d in=%zu out=%zu crc=%08lx roundtrip=%s\n", st, inB, outB, crc32(0, z, outB),
           (rc == Z_OK && bl == sizeof img && !memcmp(back, img, sizeof img)) ? "ok" : "BAD");
    free(c);
}

static void checkTtf(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { printf("ttf missing font\n"); return; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    uint8_t *font = (uint8_t *)malloc(n);
    if (fread(font, 1, n, f) != (size_t)n) n = 0;
    fclose(f);
    static uint8_t fb[296 / 8 * 128];
    TinyTTF ttf;
    ttf.setFramebuffer(296, 128, 1, fb);
    int ok = ttf.setTtfPointer(font, (uint32_t)n, 0, false);
    ttf.setCharacterSize(32);
    ttf.setCharacterSpacing(0);
    ttf.setTextColor(1, 1);
    const int width = ok ? ttf.getStringWidth("OpenEPaperLink") : -1;
    if (ok) ttf.textDraw(4, 10, "OpenEPaperLink");
    ttf.end();
    int pixels = 0;
    for (size_t i = 0; i < sizeof fb; i++) pixels += __builtin_popcount(fb[i]);
    printf("ttf loaded=%d width=%d pixels=%d crc=%08lx\n", ok, width, pixels, crc32(0, fb, sizeof fb));
    free(font);
}

int main(int argc, char **argv) {
    checkDeflate();
    checkG5();
    checkTtf(argc > 1 ? argv[1] : "ESP32_AP-Flasher/data/fonts/Signika-SB.ttf");
    return 0;
}
