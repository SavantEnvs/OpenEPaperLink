#!/usr/bin/env bash
#
# mayhem/test.sh — known-answer tests (built by mayhem/build.sh with normal flags; upstream ships none):
#   selftest      miniz-oepl deflate, G5 codec and TinyTTF rendering results vs mayhem/selftest/expected.txt
#   c6:<seed>     everything the C6/H2 radio firmware sends over serial and radio for a fixed input
#                 session (check-in, block request, cached block parts, ...) vs mayhem/c6/golden/<seed>.txt
#   v2:<seed>     everything the S3 AP's v2 transport sends, logs and queues for a fixed radio serial
#                 stream (HELLO, STATUS, chunk window, NAKs, events) vs mayhem/v2/golden/<seed>.txt
set -uo pipefail
[ -n "${SOURCE_DATE_EPOCH:-}" ] || unset SOURCE_DATE_EPOCH
cd "$SRC"

emit_ctrf() {
  local tool="$1" passed="$2" failed="$3" skipped="${4:-0}" pending="${5:-0}" other="${6:-0}"
  local tests=$(( passed + failed + skipped + pending + other ))
  cat > "${CTRF_REPORT:-$SRC/ctrf-report.json}" <<JSON
{
  "results": {
    "tool": { "name": "$tool" },
    "summary": {
      "tests": $tests,
      "passed": $passed,
      "failed": $failed,
      "pending": $pending,
      "skipped": $skipped,
      "other": $other
    }
  }
}
JSON
  printf 'CTRF {"results":{"tool":{"name":"%s"},"summary":{"tests":%d,"passed":%d,"failed":%d,"pending":%d,"skipped":%d,"other":%d}}}\n' \
    "$tool" "$tests" "$passed" "$failed" "$pending" "$skipped" "$other"
  [ "$failed" -eq 0 ]
}

T="$SRC/mayhem/out/test"
for b in oepl_selftest c6_trace c6_trace_uart v2_trace; do
  [ -x "$T/$b" ] || { echo "missing $T/$b — run mayhem/build.sh" >&2; emit_ctrf known-answer 0 1; exit 1; }
done

passed=0 failed=0
check() {  # check <name> <expected file> <command...>
  local name="$1" want="$2"; shift 2
  if diff -u "$want" <("$@" 2>&1) > /tmp/oepl-test-diff.$$; then
    echo "PASS $name"; passed=$((passed + 1))
  else
    echo "FAIL $name"; head -40 /tmp/oepl-test-diff.$$; failed=$((failed + 1))
  fi
  rm -f /tmp/oepl-test-diff.$$
}

check selftest mayhem/selftest/expected.txt "$T/oepl_selftest" ESP32_AP-Flasher/data/fonts/Signika-SB.ttf
for s in checkin_block_xfer short_checkin; do
  check "c6_radio_fw:$s" "mayhem/c6/golden/$s.txt" "$T/c6_trace" "mayhem/c6-radio-fw/testsuite/$s"
done
for s in s3_session ascii; do
  check "c6_uart_link:$s" "mayhem/c6/golden/$s.txt" "$T/c6_trace_uart" "mayhem/c6-uart-link/testsuite/$s"
done

for s in full_session nak_session wrong_version; do
  check "v2_transport:$s" "mayhem/v2/golden/$s.txt" "$T/v2_trace" "mayhem/v2-transport/testsuite/$s"
done

emit_ctrf known-answer "$passed" "$failed"
