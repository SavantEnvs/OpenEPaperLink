#!/usr/bin/env bash
#
# mayhem/build.sh — OpenEPaperLink fuzz targets and the known-answer self test.
#
# OpenEPaperLink is firmware (ESP32 PlatformIO/Arduino, ESP-IDF, Telink) with no host build, so this
# compiles the host-buildable sources directly:
#   c6_radio_fw, c6_uart_link  the C6/H2 radio firmware (ARM_Tag_FW/OpenEPaperLink_esp32_C6_AP/main:
#                              main.c, v2link.c, blockcache.c) with mayhem/c6/c6_host.c in place of the
#                              radio/UART drivers and mayhem/c6/stubs for the ESP-IDF headers (AFL, file input)
#   miniz_deflate              ESP32_AP-Flasher/lib/miniz-oepl compressor round trip (libFuzzer)
#   v2_transport               ESP32_AP-Flasher/src/v2transport.cpp, the AP's side of the protocol-v2 radio
#                              link, fed the radio's serial byte stream by mayhem/v2/v2_host.cpp (libFuzzer)
# TinyTTF (platformio.ini lib_deps nlimper/TinyTTF#6437932, cached in /opt/toolchains/deps) is an external
# library: it is built uninstrumented, for the self test only.
# Outputs go to mayhem/out/ (gitignored). Runs offline; re-runnable.
set -euo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH

: "${SANITIZER_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer}"
: "${DEBUG_FLAGS:=-g -gdwarf-3}"
: "${CC:=clang}" ; : "${CXX:=clang++}" ; : "${LIB_FUZZING_ENGINE:=-fsanitize=fuzzer}"
: "${MAYHEM_JOBS:=$(nproc)}"
: "${COVERAGE_FLAGS=}"
: "${STANDALONE_FUZZ_MAIN:=/opt/mayhem/StandaloneFuzzTargetMain.c}"
: "${TINYTTF_SRC:=/opt/toolchains/deps/tinyttf-6437932/src}"
export SANITIZER_FLAGS DEBUG_FLAGS CC CXX LIB_FUZZING_ENGINE MAYHEM_JOBS COVERAGE_FLAGS

cd "$SRC"
OUT="$SRC/mayhem/out"
OBJ="$OUT/obj"
mkdir -p "$OBJ"
C6="ARM_Tag_FW/OpenEPaperLink_esp32_C6_AP/main"
AP="ESP32_AP-Flasher"
FLAGS="$SANITIZER_FLAGS $DEBUG_FLAGS -O1"

# LeakSanitizer off for every ASan target (leaks are not the bug class fuzzed here)
$CXX $FLAGS -c mayhem/lsan_off.cc -o "$OBJ/lsan_off.o"

# 1) C6/H2 radio firmware, AFL-instrumented file-input programs (the firmware loop never returns)
C6SRC="$C6/main.c $C6/v2link.c $C6/blockcache.c mayhem/c6/c6_host.c"
afl-clang-fast $FLAGS -Imayhem/c6/stubs -I"$C6" $C6SRC "$OBJ/lsan_off.o" -o "$OUT/c6_radio_fw"
afl-clang-fast $FLAGS -DC6_UART_ONLY -Imayhem/c6/stubs -I"$C6" $C6SRC "$OBJ/lsan_off.o" -o "$OUT/c6_uart_link"

# 2) libFuzzer targets: the project code compiled with the sanitizers and SanCov, each harness linked twice
FLAGS="$FLAGS -fsanitize=fuzzer-no-link"
$CC $FLAGS -c "$STANDALONE_FUZZ_MAIN" -o "$OBJ/standalone_main.o"
$CXX $FLAGS -Imayhem/stubs -I"$AP/lib/miniz-oepl" -c "$AP/lib/miniz-oepl/miniz-oepl.cpp" -o "$OBJ/miniz-oepl.o"
V2INC="-DHAS_SUBGHZ -Imayhem/v2/stubs -I$AP/include"
$CXX $FLAGS -std=gnu++17 $V2INC -c "$AP/src/v2transport.cpp" -o "$OBJ/v2transport.o"

$CXX $FLAGS -Imayhem/stubs -I"$AP/lib/miniz-oepl" -c mayhem/harnesses/fuzz_miniz_deflate.cpp -o "$OBJ/fuzz_miniz_deflate.o"
$CXX $FLAGS -std=gnu++17 $V2INC -c mayhem/v2/v2_host.cpp -o "$OBJ/v2_host.o"

link() {  # link <name> <objects...>
    local name="$1"; shift
    $CXX $FLAGS $LIB_FUZZING_ENGINE "$@" "$OBJ/lsan_off.o" -lz -o "$OUT/$name"
    $CXX $FLAGS "$OBJ/standalone_main.o" "$@" "$OBJ/lsan_off.o" -lz -o "$OUT/$name-standalone"
}
link miniz_deflate "$OBJ/fuzz_miniz_deflate.o" "$OBJ/miniz-oepl.o"
link v2_transport "$OBJ/v2_host.o" "$OBJ/v2transport.o"

# 3) The self test and the firmware trace builds for mayhem/test.sh: normal flags, no sanitizers
T="$OUT/test"
mkdir -p "$T"
$CC  -O2 $COVERAGE_FLAGS -I"$AP/src" -Imayhem/stubs -c mayhem/selftest/g5_selftest.c -o "$T/g5_selftest.o"
$CXX -O2 $COVERAGE_FLAGS -Imayhem/stubs -I"$AP/lib/miniz-oepl" -I"$TINYTTF_SRC/src" \
    mayhem/selftest/oepl_selftest.cpp "$T/g5_selftest.o" "$AP/lib/miniz-oepl/miniz-oepl.cpp" "$TINYTTF_SRC/src/TinyTTF.cpp" \
    -lz -o "$T/oepl_selftest"
$CC -O2 $COVERAGE_FLAGS -DC6_TRACE -Imayhem/c6/stubs -I"$C6" $C6SRC -o "$T/c6_trace"
$CC -O2 $COVERAGE_FLAGS -DC6_TRACE -DC6_UART_ONLY -Imayhem/c6/stubs -I"$C6" $C6SRC -o "$T/c6_trace_uart"
$CXX -O2 $COVERAGE_FLAGS -std=gnu++17 -DV2_TRACE $V2INC "$AP/src/v2transport.cpp" mayhem/v2/v2_host.cpp -o "$T/v2_trace"
echo "build.sh: done"
