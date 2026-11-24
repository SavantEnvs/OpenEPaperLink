// Compress untrusted data the way the AP compresses tag images (makeimage.cpp, od_session.cpp):
// tdefl_initOEPL() with a limited match distance, then tdefl_compressOEPL() in steps, and inflate the
// result again with zlib (the
// vendored copy is built with MINIZ_NO_INFLATE_APIS; tags inflate the data). A round trip that does not give back the input is reported as a crash.
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "miniz-oepl.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 3) return -1;
    static const Miniz::mz_uint kDist[] = {256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
    const Miniz::mz_uint maxDist = kDist[data[0] & 7];
    const int probes = 1 + (data[0] >> 3) % 128;
    const int flags = Miniz::TDEFL_WRITE_ZLIB_HEADER | probes | ((data[1] & 1) ? Miniz::TDEFL_GREEDY_PARSING_FLAG : 0) |
                      ((data[1] & 2) ? Miniz::TDEFL_FILTER_MATCHES : 0);
    const size_t step = 1 + (size_t)data[2] * 64;  // input handed over per call, SYNC_FLUSH between steps
    data += 3; size -= 3;

    Miniz::tdefl_compressor *comp = (Miniz::tdefl_compressor *)calloc(1, sizeof(Miniz::tdefl_compressor));
    const size_t zCap = size + size / 8 + 1024;
    uint8_t *z = (uint8_t *)malloc(zCap);
    uint8_t *back = (uint8_t *)malloc(size + 1);
    if (!comp || !z || !back) { free(comp); free(z); free(back); return 0; }

    if (Miniz::tdefl_initOEPL(comp, NULL, NULL, flags, maxDist) == Miniz::TDEFL_STATUS_OKAY) {
        size_t inPos = 0, zLen = 0;
        Miniz::tdefl_status st = Miniz::TDEFL_STATUS_OKAY;
        while (st == Miniz::TDEFL_STATUS_OKAY) {
            size_t inBytes = size - inPos < step ? size - inPos : step;
            const Miniz::tdefl_flush fl = inPos + inBytes == size ? Miniz::TDEFL_FINISH : Miniz::TDEFL_SYNC_FLUSH;
            size_t outBytes = zCap - zLen;
            st = Miniz::tdefl_compressOEPL(comp, data + inPos, &inBytes, z + zLen, &outBytes, fl);
            inPos += inBytes;
            zLen += outBytes;
            if (zLen == zCap) break;
        }
        if (st == Miniz::TDEFL_STATUS_DONE) {
            uLongf backLen = size + 1;
            const int rc = uncompress(back, &backLen, z, (uLong)zLen);
            if (rc != Z_OK || backLen != size || memcmp(back, data, size) != 0) abort();
        }
    }
    free(comp); free(z); free(back);
    return 0;
}
