#!/usr/bin/env bash
# Build verification suite: compiles every firmware target and checks the
# result against what's expected, rather than just "did it succeed."
#
# notaninstrument-p4 and spike-usb-midi-idf are expected to build cleanly --
# any failure there is a real regression.
#
# spike-usb-midi-arduino is currently expected to FAIL, for a specific,
# documented reason (see its README.md: Adafruit TinyUSB's ESP32 config
# never enables CFG_TUH_MIDI). That's tracked here as an XFAIL, not
# ignored: if it starts succeeding (upstream fixed it) or starts failing
# with a *different* error, both are reported as failures of this suite,
# because both mean something changed that the docs need to catch up with.
#
# Usage:
#   ./test-builds.sh            # incremental builds (fast)
#   ./test-builds.sh --clean    # full clean rebuild of every target first
#
# Needs arduino-cli on PATH for the two Arduino targets, and either
# IDF_EXPORT_SCRIPT set or ESP-IDF installed at ~/esp/esp-idf/export.sh for
# the ESP-IDF target. Either toolchain missing SKIPs just that target
# rather than failing the suite.

set -uo pipefail

FIRMWARE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="$(mktemp -d)"

CLEAN=0
for arg in "$@"; do
  case "$arg" in
    --clean) CLEAN=1 ;;
    -h|--help)
      sed -n '2,20p' "${BASH_SOURCE[0]}"
      exit 0
      ;;
    *)
      echo "Unknown argument: $arg" >&2
      exit 2
      ;;
  esac
done

pass=0; xfail=0; fail=0; skip=0
RESULTS=()

report() {
  local name="$1" status="$2" detail="$3"
  RESULTS+=("$status|$name|$detail")
  case "$status" in
    PASS)  pass=$((pass + 1)) ;;
    XFAIL) xfail=$((xfail + 1)) ;;
    FAIL)  fail=$((fail + 1)) ;;
    SKIP)  skip=$((skip + 1)) ;;
  esac
}

# expect: "pass" -- must build cleanly, any nonzero exit is a FAIL
# expect: "known-fail" -- must fail AND contain `signature` in its output
#         (XFAIL); succeeding, or failing without that signature, is a FAIL
evaluate() {
  local name="$1" rc="$2" log="$3" expect="$4" signature="$5"
  if [ "$expect" = "pass" ]; then
    if [ "$rc" -eq 0 ]; then
      report "$name" PASS "build succeeded"
    else
      report "$name" FAIL "expected success, got exit $rc -- see $log"
    fi
  else
    if [ "$rc" -eq 0 ]; then
      report "$name" FAIL "expected known failure but build SUCCEEDED -- upstream may have fixed this; re-investigate and update README.md/CLAUDE.md (see $log)"
    elif grep -q "$signature" "$log"; then
      report "$name" XFAIL "failed with the documented known cause ('$signature') -- see README.md"
    else
      report "$name" FAIL "failed, but NOT with the documented cause ('$signature' not found) -- something else broke, see $log"
    fi
  fi
}

run_arduino_target() {
  local name="$1" dir="$2" expect="$3" signature="$4"
  local log="$LOG_DIR/$name.log"
  if ! command -v arduino-cli >/dev/null 2>&1; then
    report "$name" SKIP "arduino-cli not on PATH"
    return
  fi
  (
    cd "$dir" || exit 99
    [ "$CLEAN" -eq 1 ] && make clean >/dev/null 2>&1
    make build
  ) > "$log" 2>&1
  evaluate "$name" "$?" "$log" "$expect" "$signature"
}

run_idf_target() {
  local name="$1" dir="$2"
  local idf_export="${IDF_EXPORT_SCRIPT:-$HOME/esp/esp-idf/export.sh}"
  local log="$LOG_DIR/$name.log"
  if [ ! -f "$idf_export" ]; then
    report "$name" SKIP "ESP-IDF not found at $idf_export (set IDF_EXPORT_SCRIPT to override)"
    return
  fi
  (
    # shellcheck disable=SC1090
    source "$idf_export" > /dev/null 2>&1
    cd "$dir" || exit 99
    if [ "$CLEAN" -eq 1 ]; then
      make fullclean > /dev/null 2>&1
      rm -f sdkconfig
    fi
    [ -f sdkconfig ] || make set-target
    make build
  ) > "$log" 2>&1
  evaluate "$name" "$?" "$log" pass ""
}

run_arduino_target "notaninstrument-p4" "$FIRMWARE_DIR/notaninstrument-p4" pass ""
run_arduino_target "spike-usb-midi-arduino" "$FIRMWARE_DIR/spike-usb-midi-arduino" known-fail "tuh_midi_mount_cb_t"
run_idf_target "spike-usb-midi-idf" "$FIRMWARE_DIR/spike-usb-midi-idf"

echo
echo "=== Build verification results ==="
printf '%-6s  %-26s  %s\n' "STATUS" "TARGET" "DETAIL"
for r in "${RESULTS[@]}"; do
  IFS='|' read -r status name detail <<< "$r"
  printf '%-6s  %-26s  %s\n' "$status" "$name" "$detail"
done
echo
echo "pass=$pass xfail=$xfail fail=$fail skip=$skip  (logs kept in $LOG_DIR)"

[ "$fail" -eq 0 ]
